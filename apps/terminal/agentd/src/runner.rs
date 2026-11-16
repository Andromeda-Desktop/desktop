use std::{collections::BTreeMap, future::Future, path::PathBuf, pin::Pin, process::Stdio};

use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use tokio::{
    io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader},
    process::Command,
    sync::watch,
};

use crate::config::Config;

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct McpServer {
    pub name: String,
    pub command: String,
    pub args: Vec<String>,
    pub env: BTreeMap<String, String>,
}

#[derive(Clone, Debug)]
pub enum Event {
    Text(String),
    Thought(String),
    Tool {
        id: Option<String>,
        name: String,
        input: Value,
    },
    /// The output of a tool call; `output` is the text of the tool_result block.
    ToolResult {
        id: String,
        output: String,
        is_error: bool,
    },
    /// The system init message: the slash commands and permission mode of this run.
    Init {
        slash_commands: Vec<String>,
        mode: Option<String>,
    },
    /// Tokens in context after the last model call, and the context window when known.
    Usage { used: u64, size: Option<u64> },
    /// Claude compacts the conversation (system status "compacting") or has finished.
    Compacting(bool),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stop {
    EndTurn,
    MaxTokens,
    MaxTurns,
    Refusal,
}

#[derive(Debug)]
pub struct PermissionRequest {
    pub name: String,
    pub input: Value,
    pub tool_use_id: String,
    pub title: Option<String>,
    pub suggestions: Vec<Value>,
    pub suppress_always_allow_rule: bool,
}

pub type PermissionFuture = Pin<Box<dyn Future<Output = Value> + Send>>;

#[derive(Debug)]
pub enum RunError {
    Cancelled,
    Failed(String),
    /// The claude process exited non-zero; stderr is its trimmed log.
    Exited { code: i32, stderr: String },
}

impl std::fmt::Display for RunError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Cancelled => write!(f, "cancelled"),
            Self::Failed(s) => write!(f, "{s}"),
            Self::Exited { code, stderr } => write!(f, "claude exited with code {code}: {stderr}"),
        }
    }
}

pub struct RunRequest<'a> {
    pub cwd: &'a PathBuf,
    pub prompt: &'a str,
    pub resume: Option<&'a str>,
    pub model: &'a str,
    pub mode: &'a str,
    // A mode chosen while the run is in flight; sent to claude as set_permission_mode.
    pub mode_changes: watch::Receiver<String>,
    pub mcp_servers: &'a [McpServer],
}

pub struct RunResult {
    pub session_id: String,
    pub stop: Stop,
}

async fn write_message(
    stdin: &mut tokio::process::ChildStdin,
    value: &Value,
) -> Result<(), RunError> {
    let mut bytes = serde_json::to_vec(value).map_err(|e| RunError::Failed(e.to_string()))?;
    bytes.push(b'\n');
    stdin
        .write_all(&bytes)
        .await
        .map_err(|e| RunError::Failed(format!("failed writing to claude: {e}")))
}

// Claude's own name for the mode agentd calls "manual" is "default".
fn set_mode_request(mode: &str, serial: u64) -> Value {
    json!({
        "type": "control_request",
        "request_id": format!("agentd-mode-{serial}"),
        "request": {
            "subtype": "set_permission_mode",
            "mode": if mode == "manual" { "default" } else { mode }
        }
    })
}

pub async fn run(
    config: &Config,
    request: RunRequest<'_>,
    mut cancelled: watch::Receiver<bool>,
    mut on_event: impl FnMut(Event),
    mut on_permission: impl FnMut(PermissionRequest) -> PermissionFuture,
) -> Result<RunResult, RunError> {
    if *cancelled.borrow() {
        return Err(RunError::Cancelled);
    }
    let mut command = Command::new("claude");
    command
        .arg("-p")
        .args([
            "--output-format",
            "stream-json",
            "--input-format",
            "stream-json",
            "--verbose",
            "--include-partial-messages",
        ])
        .args(["--model", request.model]);
    if let Some(id) = request.resume {
        command.arg("--resume").arg(id);
    }
    if let Some(turns) = config.max_turns {
        command.arg("--max-turns").arg(turns.to_string());
    }
    command.args([
        "--permission-mode",
        request.mode,
        "--permission-prompt-tool",
        "stdio",
    ]);
    for tool in &config.allowed_tools {
        command.arg("--allowedTools").arg(tool);
    }

    let mut mcp_file = None;
    if !request.mcp_servers.is_empty() {
        let mut file =
            tempfile::NamedTempFile::new().map_err(|e| RunError::Failed(e.to_string()))?;
        let servers: BTreeMap<_, _> = request
            .mcp_servers
            .iter()
            .map(|s| {
                (
                    s.name.clone(),
                    json!({"command": s.command, "args": s.args, "env": s.env}),
                )
            })
            .collect();
        serde_json::to_writer(&mut file, &json!({"mcpServers": servers}))
            .map_err(|e| RunError::Failed(e.to_string()))?;
        command.arg("--mcp-config").arg(file.path());
        mcp_file = Some(file);
    }

    command
        .current_dir(request.cwd)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .env_remove("ANTHROPIC_API_KEY")
        .env_remove("ANTHROPIC_AUTH_TOKEN")
        .kill_on_drop(true);
    let mut child = command
        .spawn()
        .map_err(|e| RunError::Failed(format!("failed to start claude: {e}")))?;
    let mut stdin = child.stdin.take().expect("piped stdin");
    let user = json!({
        "type": "user",
        "session_id": "",
        "message": {"role": "user", "content": [{"type": "text", "text": request.prompt}]},
        "parent_tool_use_id": null
    });
    write_message(&mut stdin, &user).await?;
    let stdout = child.stdout.take().expect("piped stdout");
    let mut stderr = child.stderr.take().expect("piped stderr");
    let stderr_task = tokio::spawn(async move {
        let mut bytes = Vec::new();
        let _ = stderr.read_to_end(&mut bytes).await;
        String::from_utf8_lossy(&bytes).into_owned()
    });
    let mut lines = BufReader::new(stdout).lines();
    let mut session_id = String::new();
    let mut saw_text_delta = false;
    let mut saw_thought_delta = false;
    let mut result_error = None;
    let mut stop = Stop::EndTurn;
    let mut used = 0u64;
    let mut mode_changes = request.mode_changes;
    mode_changes.mark_unchanged();
    let mut mode_requests = 0u64;

    loop {
        let line = tokio::select! {
            Ok(()) = mode_changes.changed() => {
                let mode = mode_changes.borrow_and_update().clone();
                mode_requests += 1;
                write_message(&mut stdin, &set_mode_request(&mode, mode_requests)).await?;
                continue;
            }
            line = lines.next_line() => match line {
                Ok(line) => line,
                Err(e) => {
                    let _ = child.kill().await;
                    return Err(RunError::Failed(format!("failed reading claude output: {e}")));
                }
            },
            _ = cancelled.changed() => {
                if *cancelled.borrow() {
                    let _ = child.kill().await;
                    return Err(RunError::Cancelled);
                }
                continue;
            }
        };
        let Some(line) = line else {
            break;
        };
        let Ok(value) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        if let Some(id) = value.get("session_id").and_then(Value::as_str) {
            session_id = id.into();
        }
        let event = if value["type"] == "stream_event" {
            &value["event"]
        } else {
            &value
        };
        match event.get("type").and_then(Value::as_str) {
            Some("control_request") if event["request"]["subtype"] == "can_use_tool" => {
                let Some(request_id) = event["request_id"].as_str() else {
                    continue;
                };
                let permission = PermissionRequest {
                    name: event["request"]["tool_name"]
                        .as_str()
                        .unwrap_or("Tool")
                        .to_owned(),
                    input: event["request"]["input"].clone(),
                    tool_use_id: event["request"]["tool_use_id"]
                        .as_str()
                        .unwrap_or("")
                        .to_owned(),
                    title: event["request"]["title"].as_str().map(str::to_owned),
                    suggestions: event["request"]["permission_suggestions"]
                        .as_array()
                        .cloned()
                        .unwrap_or_default(),
                    suppress_always_allow_rule: event["request"]["suppress_always_allow_rule"]
                        .as_bool()
                        .unwrap_or(false),
                };
                let answer = tokio::select! {
                    answer = on_permission(permission) => answer,
                    _ = cancelled.changed() => {
                        if *cancelled.borrow() {
                            let _ = child.kill().await;
                            return Err(RunError::Cancelled);
                        }
                        continue;
                    }
                };
                write_message(
                    &mut stdin,
                    &json!({
                        "type": "control_response",
                        "response": {
                            "subtype": "success",
                            "request_id": request_id,
                            "response": answer
                        }
                    }),
                )
                .await?;
            }
            Some("control_request") => {
                if let Some(request_id) = event["request_id"].as_str() {
                    write_message(
                        &mut stdin,
                        &json!({
                            "type": "control_response",
                            "response": {
                                "subtype": "error",
                                "request_id": request_id,
                                "error": "Unsupported control request"
                            }
                        }),
                    )
                    .await?;
                }
            }
            Some("content_block_delta") => match event["delta"]["type"].as_str() {
                Some("text_delta") => {
                    if let Some(text) = event["delta"]["text"].as_str() {
                        saw_text_delta = true;
                        on_event(Event::Text(text.into()));
                    }
                }
                Some("thinking_delta") => {
                    if let Some(text) = event["delta"]["thinking"].as_str() {
                        saw_thought_delta = true;
                        on_event(Event::Thought(text.into()));
                    }
                }
                _ => {}
            },
            Some("system") if value["subtype"] == "status" => {
                on_event(Event::Compacting(value["status"] == "compacting"))
            }
            Some("system") if value["subtype"] == "compact_boundary" => on_event(Event::Compacting(false)),
            Some("system") if value["subtype"] == "init" => {
                on_event(Event::Init {
                    slash_commands: value["slash_commands"]
                        .as_array()
                        .map(|names| {
                            names
                                .iter()
                                .filter_map(Value::as_str)
                                .map(str::to_owned)
                                .collect()
                        })
                        .unwrap_or_default(),
                    mode: value["permissionMode"].as_str().map(str::to_owned),
                });
            }
            Some("user") => {
                for block in value["message"]["content"].as_array().into_iter().flatten() {
                    if block["type"] != "tool_result" {
                        continue;
                    }
                    let Some(id) = block["tool_use_id"].as_str() else {
                        continue;
                    };
                    on_event(Event::ToolResult {
                        id: id.to_owned(),
                        output: tool_result_text(&block["content"]),
                        is_error: block["is_error"].as_bool().unwrap_or(false),
                    });
                }
            }
            Some("assistant") => {
                let usage = &value["message"]["usage"];
                let tokens = ["input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"]
                    .iter()
                    .filter_map(|key| usage[key].as_u64())
                    .sum::<u64>();
                if tokens > 0 {
                    used = tokens;
                }
                match value["message"]["stop_reason"].as_str() {
                    Some("max_tokens") => stop = Stop::MaxTokens,
                    Some("refusal") => stop = Stop::Refusal,
                    Some(_) => stop = Stop::EndTurn,
                    None => {}
                }
                if let Some(blocks) = value["message"]["content"].as_array() {
                    for block in blocks {
                        match block["type"].as_str() {
                            Some("text") if !saw_text_delta => {
                                if let Some(text) = block["text"].as_str() {
                                    on_event(Event::Text(text.into()));
                                }
                            }
                            Some("thinking") if !saw_thought_delta => {
                                if let Some(text) = block["thinking"].as_str() {
                                    on_event(Event::Thought(text.into()));
                                }
                            }
                            Some("tool_use") => {
                                if let Some(name) = block["name"].as_str() {
                                    on_event(Event::Tool {
                                        id: block["id"].as_str().map(str::to_owned),
                                        name: name.into(),
                                        input: block["input"].clone(),
                                    });
                                }
                            }
                            _ => {}
                        }
                    }
                }
                saw_text_delta = false;
                saw_thought_delta = false;
            }
            Some("result") if value["subtype"] == "error_max_turns" => stop = Stop::MaxTurns,
            Some("result") if value["is_error"].as_bool() == Some(true) => {
                result_error = Some(
                    value["result"]
                        .as_str()
                        .unwrap_or("claude reported an error")
                        .to_owned(),
                );
            }
            _ => {}
        }
        if event["type"] == "result" {
            if used > 0 {
                let size = value["modelUsage"]
                    .as_object()
                    .and_then(|models| models.values().find_map(|m| m["contextWindow"].as_u64()));
                on_event(Event::Usage { used, size });
            }
            break;
        }
    }
    drop(stdin);
    let status = tokio::select! {
        status = child.wait() => status
            .map_err(|e| RunError::Failed(format!("claude process failed: {e}")))?,
        _ = cancelled.changed() => {
            if *cancelled.borrow() {
                let _ = child.kill().await;
                return Err(RunError::Cancelled);
            }
            child.wait().await.map_err(|e| RunError::Failed(format!("claude process failed: {e}")))?
        }
    };
    let stderr = stderr_task.await.unwrap_or_default();
    drop(mcp_file);
    if let Some(error) = result_error {
        return Err(RunError::Failed(error));
    }
    if !status.success() {
        return Err(RunError::Exited {
            code: status.code().unwrap_or(-1),
            stderr: stderr.trim().to_string(),
        });
    }
    Ok(RunResult { session_id, stop })
}

/// The text of a tool_result `content`: a string, or text blocks joined by newlines.
fn tool_result_text(content: &Value) -> String {
    match content {
        Value::String(text) => text.clone(),
        Value::Array(blocks) => blocks
            .iter()
            .filter_map(|block| block["text"].as_str())
            .collect::<Vec<_>>()
            .join("\n"),
        _ => String::new(),
    }
}
