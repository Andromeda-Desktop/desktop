use std::{collections::BTreeMap, future::Future, path::PathBuf, pin::Pin, process::Stdio};

use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use tokio::{
    io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader},
    process::Command,
    sync::watch,
    time::{Instant, timeout_at},
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
}

impl std::fmt::Display for RunError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Cancelled => write!(f, "cancelled"),
            Self::Failed(s) => write!(f, "{s}"),
        }
    }
}

pub struct RunRequest<'a> {
    pub cwd: &'a PathBuf,
    pub prompt: &'a str,
    pub resume: Option<&'a str>,
    pub model: &'a str,
    pub mode: &'a str,
    pub mcp_servers: &'a [McpServer],
}

pub struct RunResult {
    pub session_id: String,
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
    let deadline = Instant::now() + config.timeout;
    let mut session_id = String::new();
    let mut saw_text_delta = false;
    let mut saw_thought_delta = false;
    let mut result_error = None;

    loop {
        let line = tokio::select! {
            line = timeout_at(deadline, lines.next_line()) => match line {
                Ok(Ok(line)) => line,
                Ok(Err(e)) => {
                    let _ = child.kill().await;
                    return Err(RunError::Failed(format!("failed reading claude output: {e}")));
                }
                Err(_) => {
                    let _ = child.kill().await;
                    return Err(RunError::Failed("claude timed out".into()));
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
            Some("assistant") => {
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
            break;
        }
    }
    drop(stdin);
    let status = tokio::select! {
        status = timeout_at(deadline, child.wait()) => match status {
            Ok(Ok(status)) => status,
            Ok(Err(e)) => return Err(RunError::Failed(format!("claude process failed: {e}"))),
            Err(_) => {
                let _ = child.kill().await;
                return Err(RunError::Failed("claude timed out".into()));
            }
        },
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
        return Err(RunError::Failed(format!(
            "claude exited with {status}: {}",
            stderr.trim()
        )));
    }
    Ok(RunResult { session_id })
}
