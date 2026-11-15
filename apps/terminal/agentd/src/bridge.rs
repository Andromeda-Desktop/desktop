use std::{
    collections::{HashMap, HashSet},
    path::Path,
    sync::{Arc, Mutex},
};

use agent_client_protocol::schema::v1::{
    self as acp, ContentBlock, ContentChunk, PermissionOption, PermissionOptionKind,
    RequestPermissionRequest, SessionConfigOption, SessionConfigOptionCategory,
    SessionConfigSelectOption, SessionNotification, SessionUpdate, StopReason, TextContent,
    ToolCall, ToolCallContent, ToolCallStatus, ToolKind,
};
use agent_client_protocol::{Client, ConnectionTo, Error};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use serde_json::{Value, json};
use tokio::sync::watch;
use uuid::Uuid;

use crate::{
    config::Config,
    history::History,
    runner::{self, Event, McpServer, PermissionRequest, RunError, RunRequest},
    store::{Session, Store},
    validation,
};

pub struct Bridge {
    config: Config,
    store: Store,
    history: History,
    running: Mutex<HashMap<String, watch::Sender<bool>>>,
}

impl Bridge {
    pub fn new(config: Config) -> Arc<Self> {
        Arc::new(Self {
            store: Store::new(config.store_dir.clone()),
            history: History::new(config.claude_dir.clone()),
            config,
            running: Mutex::new(HashMap::new()),
        })
    }

    pub fn initialize(&self, request: acp::InitializeRequest) -> acp::InitializeResponse {
        acp::InitializeResponse::new(request.protocol_version)
            .agent_info(
                acp::Implementation::new("andromeda-agentd", env!("CARGO_PKG_VERSION"))
                    .title("Claude Code ACP Bridge"),
            )
            .agent_capabilities(
                acp::AgentCapabilities::new()
                    .load_session(true)
                    .session_capabilities(
                        acp::SessionCapabilities::new()
                            .list(acp::SessionListCapabilities::new())
                            .resume(acp::SessionResumeCapabilities::new()),
                    ),
            )
    }

    pub fn new_session(
        &self,
        request: acp::NewSessionRequest,
    ) -> std::result::Result<acp::NewSessionResponse, Error> {
        let cwd = validation::cwd(&request.cwd).map_err(invalid)?;
        let servers = parse_mcp(request.mcp_servers)?;
        let id = Uuid::new_v4().simple().to_string();
        let session = Session::new(cwd, servers);
        self.store.create(id.clone(), session.clone(), true);
        Ok(acp::NewSessionResponse::new(id).config_options(self.options(&session)))
    }

    pub fn load_session(
        &self,
        request: acp::LoadSessionRequest,
    ) -> std::result::Result<acp::LoadSessionResponse, Error> {
        let session = self.require(&request.session_id.to_string())?;
        Ok(acp::LoadSessionResponse::new().config_options(self.options(&session)))
    }

    pub fn list_sessions(
        &self,
        request: acp::ListSessionsRequest,
    ) -> std::result::Result<acp::ListSessionsResponse, Error> {
        let mut entries: Vec<acp::SessionInfo> = self
            .history
            .list(request.cwd.as_deref())
            .into_iter()
            .map(|entry| {
                acp::SessionInfo::new(entry.id, entry.cwd)
                    .title(entry.title)
                    .updated_at(entry.updated_at)
            })
            .collect();
        let native: HashSet<String> = entries
            .iter()
            .map(|entry| entry.session_id.to_string())
            .collect();
        for (id, session) in self.store.list(request.cwd.as_deref()) {
            if !native.contains(&id)
                && !session
                    .claude_session_id
                    .as_ref()
                    .is_some_and(|id| native.contains(id))
            {
                entries.push(
                    acp::SessionInfo::new(id, session.cwd)
                        .title(session.title)
                        .updated_at(session.updated_at),
                );
            }
        }
        entries.sort_by(|a, b| {
            b.updated_at
                .cmp(&a.updated_at)
                .then(a.session_id.to_string().cmp(&b.session_id.to_string()))
        });
        let offset = if let Some(cursor) = request.cursor {
            let id = URL_SAFE_NO_PAD
                .decode(cursor)
                .ok()
                .and_then(|s| String::from_utf8(s).ok())
                .ok_or_else(|| invalid("Invalid session list cursor"))?;
            entries
                .iter()
                .position(|entry| entry.session_id.to_string() == id)
                .map(|index| index + 1)
                .ok_or_else(|| invalid("Invalid session list cursor"))?
        } else {
            0
        };
        let next = if offset + 50 < entries.len() {
            entries
                .get(offset + 49)
                .map(|entry| URL_SAFE_NO_PAD.encode(entry.session_id.to_string()))
        } else {
            None
        };
        Ok(
            acp::ListSessionsResponse::new(entries.into_iter().skip(offset).take(50).collect())
                .next_cursor(next),
        )
    }

    pub fn resume_session(
        &self,
        request: acp::ResumeSessionRequest,
    ) -> std::result::Result<acp::ResumeSessionResponse, Error> {
        let id = request.session_id.to_string();
        let cwd = validation::cwd(&request.cwd).map_err(invalid)?;
        let servers = parse_mcp(request.mcp_servers)?;
        if let Some(session) = self.store.get(&id) {
            if session.cwd != cwd {
                return Err(invalid("Session cwd does not match"));
            }
            self.store.update(&id, |session| {
                if !servers.is_empty() {
                    session.mcp_servers = servers;
                }
                if self.history.find(&id, &cwd) {
                    session.native = true;
                }
            });
        } else if self.history.find(&id, &cwd) {
            let mut session = Session::new(cwd, servers);
            session.native = true;
            session.claude_session_id = Some(id.clone());
            self.store.create(id.clone(), session, false);
        } else if id.len() == 32 && id.bytes().all(|b| b.is_ascii_hexdigit()) {
            self.store
                .create(id.clone(), Session::new(cwd, servers), true);
        } else {
            return Err(Error::new(
                -32002,
                format!("Claude Code session {id} not found"),
            ));
        }
        let session = self.require(&id)?;
        Ok(acp::ResumeSessionResponse::new().config_options(self.options(&session)))
    }

    pub fn set_config(
        &self,
        request: acp::SetSessionConfigOptionRequest,
        connection: &ConnectionTo<Client>,
    ) -> std::result::Result<acp::SetSessionConfigOptionResponse, Error> {
        let id = request.session_id.to_string();
        self.require(&id)?;
        let key = request.config_id.to_string();
        let value = request
            .value
            .as_value_id()
            .ok_or_else(|| invalid("Expected a select value"))?
            .to_string();
        match key.as_str() {
            "mode" if is_mode(&value) => {
                self.store
                    .update(&id, |session| session.mode = Some(value.clone()));
            }
            "model" => {
                validation::model_id(&value).map_err(invalid)?;
                if !self
                    .model_ids(&self.require(&id)?)
                    .iter()
                    .any(|candidate| candidate == &value)
                {
                    return Err(invalid(format!("Invalid model: {value}")));
                }
                self.store
                    .update(&id, |session| session.model = Some(value));
            }
            "thought_level" if ["low", "medium", "high", "max"].contains(&value.as_str()) => {
                self.store.update(&id, |session| {
                    session.config_overrides.insert(key, json!(value));
                });
            }
            _ => return Err(invalid("Unknown configuration option or value")),
        }
        let options = self.options(&self.require(&id)?);
        notify(
            connection,
            &id,
            SessionUpdate::ConfigOptionUpdate(acp::ConfigOptionUpdate::new(options.clone())),
        );
        Ok(acp::SetSessionConfigOptionResponse::new(options))
    }

    pub fn cancel(&self, id: &str) {
        if let Some(sender) = self.running.lock().unwrap().get(id) {
            let _ = sender.send(true);
        }
    }

    pub async fn prompt(
        &self,
        request: acp::PromptRequest,
        connection: ConnectionTo<Client>,
    ) -> std::result::Result<acp::PromptResponse, Error> {
        let id = request.session_id.to_string();
        let session = self.require(&id)?;
        let prompt = request
            .prompt
            .iter()
            .filter_map(|block| match block {
                ContentBlock::Text(text) => Some(text.text.as_str()),
                _ => None,
            })
            .collect::<Vec<_>>()
            .join("\n");
        if prompt.trim().is_empty() {
            return Err(invalid("Empty prompt text"));
        }
        let (cancel_tx, cancel_rx) = watch::channel(false);
        {
            let mut running = self.running.lock().unwrap();
            if running.contains_key(&id) {
                return Err(invalid("A prompt is already running for this session"));
            }
            running.insert(id.clone(), cancel_tx);
        }

        if session.title.is_none() {
            let title: String = prompt
                .split_whitespace()
                .collect::<Vec<_>>()
                .join(" ")
                .chars()
                .take(120)
                .collect();
            self.store
                .update(&id, |session| session.title = Some(title.clone()));
            notify(
                &connection,
                &id,
                SessionUpdate::SessionInfoUpdate(acp::SessionInfoUpdate::new().title(title)),
            );
        }

        let mut tool_counter = 0;
        let seen_tool_ids = Mutex::new(HashSet::new());
        let result = {
            let mut on_event = |event: Event| match event {
                Event::Text(text) => notify(
                    &connection,
                    &id,
                    SessionUpdate::AgentMessageChunk(ContentChunk::new(ContentBlock::Text(
                        TextContent::new(text),
                    ))),
                ),
                Event::Thought(text) => notify(
                    &connection,
                    &id,
                    SessionUpdate::AgentThoughtChunk(ContentChunk::new(ContentBlock::Text(
                        TextContent::new(text),
                    ))),
                ),
                Event::Tool {
                    id: tool_use_id,
                    name,
                    input,
                } => {
                    tool_counter += 1;
                    let tool_id = tool_use_id.unwrap_or_else(|| format!("call_{tool_counter}"));
                    seen_tool_ids.lock().unwrap().insert(tool_id.clone());
                    let tool = ToolCall::new(tool_id, tool_title(&name, &input))
                        .name(name)
                        .kind(ToolKind::Execute)
                        .status(ToolCallStatus::Completed)
                        .raw_input(input);
                    notify(&connection, &id, SessionUpdate::ToolCall(tool));
                }
            };
            let on_permission = |permission: PermissionRequest| -> runner::PermissionFuture {
                let tool_id = if permission.tool_use_id.is_empty() {
                    format!("permission_{}", Uuid::new_v4())
                } else {
                    permission.tool_use_id.clone()
                };
                let title = tool_title(&permission.name, &permission.input);
                let title = if title == permission.name {
                    permission.title.clone().unwrap_or(title)
                } else {
                    title
                };
                let content = vec![ToolCallContent::from(ContentBlock::Text(TextContent::new(
                    permission_content(&permission, &session.cwd),
                )))];
                let fields = acp::ToolCallUpdateFields::new()
                    .title(title.clone())
                    .name(permission.name.clone())
                    .kind(ToolKind::Execute)
                    .status(ToolCallStatus::Pending)
                    .raw_input(permission.input.clone())
                    .content(content.clone());
                let update = acp::ToolCallUpdate::new(tool_id.clone(), fields);
                if seen_tool_ids.lock().unwrap().insert(tool_id.clone()) {
                    let tool = ToolCall::new(tool_id.clone(), title)
                        .name(permission.name.clone())
                        .kind(ToolKind::Execute)
                        .status(ToolCallStatus::Pending)
                        .raw_input(permission.input.clone())
                        .content(content);
                    notify(&connection, &id, SessionUpdate::ToolCall(tool));
                } else {
                    notify(
                        &connection,
                        &id,
                        SessionUpdate::ToolCallUpdate(update.clone()),
                    );
                }
                let mut options = vec![PermissionOption::new(
                    "allow_once",
                    "Allow once",
                    PermissionOptionKind::AllowOnce,
                )];
                if !permission.suggestions.is_empty() && !permission.suppress_always_allow_rule {
                    options.push(PermissionOption::new(
                        "allow_always",
                        "Allow always",
                        PermissionOptionKind::AllowAlways,
                    ));
                }
                options.push(PermissionOption::new(
                    "reject_once",
                    "Reject once",
                    PermissionOptionKind::RejectOnce,
                ));
                let request = connection.send_request(RequestPermissionRequest::new(
                    id.clone(),
                    update,
                    options,
                ));
                let connection = connection.clone();
                let id = id.clone();
                Box::pin(async move {
                    let outcome = request.block_task().await;
                    let selected = match outcome {
                        Ok(response) => match response.outcome {
                            acp::RequestPermissionOutcome::Selected(selection) => {
                                Some(selection.option_id.to_string())
                            }
                            acp::RequestPermissionOutcome::Cancelled => None,
                            _ => None,
                        },
                        Err(_) => None,
                    };
                    let approved =
                        matches!(selected.as_deref(), Some("allow_once" | "allow_always"));
                    notify(
                        &connection,
                        &id,
                        SessionUpdate::ToolCallUpdate(acp::ToolCallUpdate::new(
                            tool_id,
                            acp::ToolCallUpdateFields::new().status(if approved {
                                ToolCallStatus::Completed
                            } else {
                                ToolCallStatus::Failed
                            }),
                        )),
                    );
                    match selected.as_deref() {
                        Some("allow_once") => json!({
                            "behavior": "allow",
                            "updatedInput": permission.input,
                            "toolUseID": permission.tool_use_id,
                            "decisionClassification": "user_temporary"
                        }),
                        Some("allow_always")
                            if !permission.suggestions.is_empty()
                                && !permission.suppress_always_allow_rule =>
                        {
                            json!({
                                "behavior": "allow",
                                "updatedInput": permission.input,
                                "updatedPermissions": permission.suggestions,
                                "toolUseID": permission.tool_use_id,
                                "decisionClassification": "user_permanent"
                            })
                        }
                        _ => json!({
                            "behavior": "deny",
                            "message": "Permission denied in Zed",
                            "toolUseID": permission.tool_use_id,
                            "decisionClassification": "user_reject"
                        }),
                    }
                })
            };

            let model = session
                .model
                .as_deref()
                .unwrap_or(&self.config.model)
                .to_owned();
            if let Some(claude_id) = &session.claude_session_id {
                match runner::run(
                    &self.config,
                    run_request(&session, &prompt, &model, Some(claude_id)),
                    cancel_rx.clone(),
                    &mut on_event,
                    on_permission,
                )
                .await
                {
                    Err(RunError::Failed(_)) if !session.native => {
                        self.store.clear_persisted(Path::new(&session.cwd));
                        runner::run(
                            &self.config,
                            run_request(&session, &prompt, &model, None),
                            cancel_rx,
                            &mut on_event,
                            on_permission,
                        )
                        .await
                    }
                    result => result,
                }
            } else {
                runner::run(
                    &self.config,
                    run_request(&session, &prompt, &model, None),
                    cancel_rx,
                    &mut on_event,
                    on_permission,
                )
                .await
            }
        };
        self.running.lock().unwrap().remove(&id);
        match result {
            Ok(result) => {
                self.store.set_claude_id(&id, result.session_id, true);
                if let Some(session) = self.store.get(&id) {
                    notify(
                        &connection,
                        &id,
                        SessionUpdate::SessionInfoUpdate(
                            acp::SessionInfoUpdate::new().updated_at(session.updated_at),
                        ),
                    );
                }
                Ok(acp::PromptResponse::new(StopReason::EndTurn))
            }
            Err(RunError::Cancelled) => Ok(acp::PromptResponse::new(StopReason::Cancelled)),
            Err(RunError::Failed(error)) => {
                notify(
                    &connection,
                    &id,
                    SessionUpdate::AgentMessageChunk(ContentChunk::new(ContentBlock::Text(
                        TextContent::new(format!("Error: {error}")),
                    ))),
                );
                Ok(acp::PromptResponse::new(StopReason::EndTurn))
            }
        }
    }

    fn require(&self, id: &str) -> std::result::Result<Session, Error> {
        self.store
            .get(id)
            .ok_or_else(|| Error::new(-32002, format!("Session {id} not found")))
    }

    fn options(&self, session: &Session) -> Vec<SessionConfigOption> {
        let mode = session_mode(session);
        let model = session.model.as_deref().unwrap_or(&self.config.model);
        let model_options = self
            .model_ids(session)
            .into_iter()
            .map(|id| {
                let name = match id.as_str() {
                    "claude-fable-5-1" => "Fable 5.1",
                    "claude-fable-5" => "Fable 5",
                    "claude-opus-5-5" => "Opus 5.5",
                    "claude-opus-5" => "Opus 5",
                    "claude-opus-4-8" => "Opus 4.8",
                    "claude-sonnet-5" => "Sonnet 5",
                    "claude-sonnet-4-6" => "Sonnet 4.6",
                    "claude-haiku-4-5-20251001" => "Haiku 4.5",
                    _ => id.as_str(),
                };
                SessionConfigSelectOption::new(id.clone(), name.to_owned())
            })
            .collect::<Vec<_>>();
        let thought = session
            .config_overrides
            .get("thought_level")
            .and_then(Value::as_str)
            .unwrap_or("medium");
        vec![
            SessionConfigOption::select("model", "Model", model.to_owned(), model_options)
                .category(SessionConfigOptionCategory::Model),
            SessionConfigOption::select(
                "thought_level",
                "Thinking Level",
                thought.to_owned(),
                vec![
                    SessionConfigSelectOption::new("low", "Low"),
                    SessionConfigSelectOption::new("medium", "Medium"),
                    SessionConfigSelectOption::new("high", "High"),
                    SessionConfigSelectOption::new("max", "Max"),
                ],
            )
            .category(SessionConfigOptionCategory::ThoughtLevel)
            .description("Control the depth of reasoning"),
            SessionConfigOption::select("mode", "Mode", mode.to_owned(), mode_options())
                .category(SessionConfigOptionCategory::Mode),
        ]
    }

    fn model_ids(&self, session: &Session) -> Vec<String> {
        let mut models: Vec<String> = [
            "claude-fable-5-1",
            "claude-fable-5",
            "claude-opus-5-5",
            "claude-opus-5",
            "claude-opus-4-8",
            "claude-sonnet-5",
            "claude-sonnet-4-6",
            "claude-haiku-4-5-20251001",
        ]
        .into_iter()
        .map(str::to_owned)
        .collect();
        for model in [
            &self.config.model,
            session.model.as_ref().unwrap_or(&self.config.model),
        ] {
            if !models.contains(model) {
                models.push(model.clone());
            }
        }
        models
    }
}

fn is_mode(value: &str) -> bool {
    ["auto", "manual", "acceptEdits", "plan"].contains(&value)
}

fn session_mode(session: &Session) -> &str {
    session
        .mode
        .as_deref()
        .filter(|mode| is_mode(mode))
        .unwrap_or("manual")
}

fn mode_options() -> Vec<SessionConfigSelectOption> {
    [
        ("auto", "Auto", "Let Claude assess actions automatically"),
        ("manual", "Manual", "Ask before edits and commands"),
        (
            "acceptEdits",
            "Accept edits",
            "Accept file edits automatically",
        ),
        ("plan", "Plan", "Explore and plan without editing files"),
    ]
    .into_iter()
    .map(|(id, name, description)| {
        SessionConfigSelectOption::new(id, name).description(description)
    })
    .collect()
}

fn tool_title(name: &str, input: &Value) -> String {
    let detail = if name == "Bash" {
        input.get("command").and_then(Value::as_str)
    } else {
        input
            .get("file_path")
            .or_else(|| input.get("path"))
            .and_then(Value::as_str)
    };
    let Some(first_line) = detail.and_then(|text| text.trim().lines().next()) else {
        return name.to_owned();
    };
    let mut title: String = first_line.chars().take(160).collect();
    if first_line.chars().count() > 160 {
        title.push('…');
    }
    if title.is_empty() {
        name.to_owned()
    } else if name == "Bash" {
        title
    } else {
        format!("{name} {title}")
    }
}

fn permission_content(permission: &PermissionRequest, cwd: &Path) -> String {
    let detail = match permission.input.get("command").and_then(Value::as_str) {
        Some(command) if permission.name == "Bash" => {
            format!("Command:\n\n{command}")
        }
        _ => format!(
            "Arguments:\n\n{}",
            serde_json::to_string_pretty(&permission.input).unwrap_or_default()
        ),
    };
    format!("Working directory: {}\n\n{detail}", cwd.display())
}

fn run_request<'a>(
    session: &'a Session,
    prompt: &'a str,
    model: &'a str,
    resume: Option<&'a str>,
) -> RunRequest<'a> {
    RunRequest {
        cwd: &session.cwd,
        prompt,
        resume,
        model,
        mode: session_mode(session),
        mcp_servers: &session.mcp_servers,
    }
}

fn parse_mcp(servers: Vec<acp::McpServer>) -> std::result::Result<Vec<McpServer>, Error> {
    servers
        .into_iter()
        .filter_map(|server| match server {
            acp::McpServer::Stdio(server) => Some(server),
            _ => None,
        })
        .map(|server| {
            validation::mcp_command(&server.command).map_err(invalid)?;
            for arg in &server.args {
                validation::mcp_arg(arg).map_err(invalid)?;
            }
            Ok(McpServer {
                name: server.name,
                command: server.command.to_string_lossy().into_owned(),
                args: server.args,
                env: server
                    .env
                    .into_iter()
                    .map(|var| (var.name, var.value))
                    .collect(),
            })
        })
        .collect()
}

fn invalid(message: impl ToString) -> Error {
    Error::new(-32602, message.to_string())
}

fn notify(connection: &ConnectionTo<Client>, id: &str, update: SessionUpdate) {
    if let Err(error) =
        connection.send_notification(SessionNotification::new(id.to_owned(), update))
    {
        eprintln!("failed to send session update: {error}");
    }
}
