#![cfg(unix)]

use std::{fs, os::unix::fs::PermissionsExt, path::Path, process::Stdio};

use serde_json::{Value, json};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader},
    process::{Child, ChildStdout, Command},
    time::{Duration, timeout},
};

fn fake_claude(dir: &Path, script: &str) {
    let path = dir.join("claude");
    fs::write(&path, format!("#!/bin/sh\n{script}\n")).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
}

fn start(dir: &Path, store: &Path) -> (Child, BufReader<ChildStdout>) {
    let mut child = Command::new(env!("CARGO_BIN_EXE_andromeda-agentd"))
        .env(
            "PATH",
            format!("{}:{}", dir.display(), std::env::var("PATH").unwrap()),
        )
        .env("CLAUDE_ACP_STORE_DIR", store)
        .env(
            "CLAUDE_CONFIG_DIR",
            dir.parent().unwrap().join("empty-claude"),
        )
        .env("ANTHROPIC_API_KEY", "must-be-removed")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let reader = BufReader::new(child.stdout.take().unwrap());
    (child, reader)
}

async fn send(child: &mut Child, message: Value) {
    let stdin = child.stdin.as_mut().unwrap();
    stdin
        .write_all(message.to_string().as_bytes())
        .await
        .unwrap();
    stdin.write_all(b"\n").await.unwrap();
}

async fn receive(reader: &mut BufReader<ChildStdout>) -> Value {
    let mut line = String::new();
    timeout(Duration::from_secs(5), reader.read_line(&mut line))
        .await
        .expect("timed out waiting for ACP response")
        .unwrap();
    assert!(!line.is_empty(), "ACP process closed stdout");
    serde_json::from_str(&line).unwrap()
}

async fn response(reader: &mut BufReader<ChildStdout>, id: i64) -> (Value, Vec<Value>) {
    let mut notifications = Vec::new();
    loop {
        let value = receive(reader).await;
        if value["id"] == id {
            return (value, notifications);
        }
        notifications.push(value);
    }
}

async fn initialize(child: &mut Child, reader: &mut BufReader<ChildStdout>) {
    send(
        child,
        json!({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
            "protocolVersion":1,"clientCapabilities":{},"clientInfo":{"name":"test","version":"1"}
        }}),
    )
    .await;
    let (value, _) = response(reader, 1).await;
    assert_eq!(value["result"]["protocolVersion"], 1);
    assert_eq!(value["result"]["agentCapabilities"]["loadSession"], true);
}

#[tokio::test]
async fn streams_and_resumes_a_persisted_session() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    let store = temp.path().join("store");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
if [ -n "$ANTHROPIC_API_KEY" ] || [ -n "$ANTHROPIC_AUTH_TOKEN" ]; then exit 11; fi
mode=''
while [ "$#" -gt 0 ]; do
  if [ "$1" = '--permission-mode' ]; then mode="$2"; break; fi
  shift
done
if [ "$mode" != 'plan' ]; then exit 12; fi
printf '%s\n' '{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"hello "}}}'
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"hello world"},{"type":"tool_use","name":"Read","input":{"file_path":"x"}}]}}'
printf '%s\n' '{"type":"result","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"hello world"}'
"#,
    );
    let (mut child, mut reader) = start(&bin, &store);
    initialize(&mut child, &mut reader).await;
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{
            "cwd":project,"mcpServers":[]
        }}),
    )
    .await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    let mode = new["result"]["configOptions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|option| option["category"] == "mode")
        .expect("mode config option");
    assert_eq!(mode["id"], "mode");
    assert_eq!(mode["currentValue"], "manual");
    assert_eq!(mode["options"].as_array().unwrap().len(), 4);
    assert_eq!(
        new["result"]["configOptions"][0]["currentValue"],
        "claude-opus-5-5"
    );

    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":7,"method":"session/set_config_option","params":{
            "sessionId":id,"configId":"mode","value":"plan"
        }}),
    )
    .await;
    let (set_mode, mode_updates) = response(&mut reader, 7).await;
    let mode = set_mode["result"]["configOptions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|option| option["id"] == "mode")
        .unwrap();
    assert_eq!(mode["currentValue"], "plan");
    assert!(
        mode_updates.iter().any(|update| {
            update["params"]["update"]["sessionUpdate"] == "config_option_update"
        })
    );

    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":3,"method":"session/set_config_option","params":{
            "sessionId":id,"configId":"model","value":"claude-opus-5-5"
        }}),
    )
    .await;
    let (set, _) = response(&mut reader, 3).await;
    assert_eq!(
        set["result"]["configOptions"][0]["currentValue"],
        "claude-opus-5-5"
    );

    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":4,"method":"session/prompt","params":{
            "sessionId":id,"prompt":[{"type":"text","text":"say hello"}]
        }}),
    )
    .await;
    let (prompt, updates) = response(&mut reader, 4).await;
    assert_eq!(prompt["result"]["stopReason"], "end_turn");
    let texts: Vec<_> = updates
        .iter()
        .filter(|v| v["params"]["update"]["sessionUpdate"] == "agent_message_chunk")
        .map(|v| v["params"]["update"]["content"]["text"].as_str().unwrap())
        .collect();
    assert_eq!(texts, ["hello "]);
    assert!(
        updates
            .iter()
            .any(|v| v["params"]["update"]["sessionUpdate"] == "tool_call")
    );
    assert!(store.join("acp-sessions.json").exists());
    child.kill().await.unwrap();

    let (mut child, mut reader) = start(&bin, &store);
    initialize(&mut child, &mut reader).await;
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":5,"method":"session/resume","params":{
            "sessionId":id,"cwd":project,"mcpServers":[]
        }}),
    )
    .await;
    let (resume, _) = response(&mut reader, 5).await;
    assert_eq!(
        resume["result"]["configOptions"][0]["currentValue"],
        "claude-opus-5-5"
    );
    assert_eq!(resume["result"]["configOptions"][2]["currentValue"], "plan");
    child.kill().await.unwrap();
}

#[tokio::test]
async fn selected_mode_reaches_claude() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
mode='missing'
while [ "$#" -gt 0 ]; do
  if [ "$1" = '--permission-mode' ]; then mode="$2"; break; fi
  shift
done
printf '{"type":"assistant","message":{"content":[{"type":"text","text":"%s"}]}}\n' "$mode"
printf '%s\n' '{"type":"result","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"done"}'
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();

    for (index, mode) in ["manual", "auto", "acceptEdits", "plan"].iter().enumerate() {
        let request_id = 3 + index as i64 * 2;
        if index > 0 {
            send(&mut child, json!({"jsonrpc":"2.0","id":request_id,"method":"session/set_config_option","params":{"sessionId":id,"configId":"mode","value":mode}})).await;
            let (set, _) = response(&mut reader, request_id).await;
            assert_eq!(set["result"]["configOptions"][2]["currentValue"], *mode);
        }
        let prompt_id = request_id + 1;
        send(&mut child, json!({"jsonrpc":"2.0","id":prompt_id,"method":"session/prompt","params":{"sessionId":id,"prompt":[{"type":"text","text":"say mode"}]}})).await;
        let (prompt, updates) = response(&mut reader, prompt_id).await;
        assert_eq!(prompt["result"]["stopReason"], "end_turn", "{prompt}");
        assert!(
            updates
                .iter()
                .any(|update| update["params"]["update"]["content"]["text"] == *mode),
            "missing {mode} in {updates:?}"
        );
    }
    child.kill().await.unwrap();
}

#[tokio::test]
async fn a_mode_chosen_during_a_run_reaches_the_running_claude() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
read -r user
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"started"}]}}'
while read -r line; do
  mode=$(printf '%s' "$line" | sed -n 's/.*"subtype":"set_permission_mode","mode":"\([a-zA-Z]*\)".*/\1/p')
  if [ -n "$mode" ]; then break; fi
done
printf '{"type":"assistant","message":{"content":[{"type":"text","text":"now %s"}]}}\n' "$mode"
printf '%s\n' '{"type":"result","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"done"}'
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    send(&mut child, json!({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":id,"prompt":[{"type":"text","text":"work"}]}})).await;
    loop {
        let value = receive(&mut reader).await;
        if value["params"]["update"]["content"]["text"] == "started" {
            break;
        }
    }
    send(&mut child, json!({"jsonrpc":"2.0","id":4,"method":"session/set_config_option","params":{"sessionId":id,"configId":"mode","value":"auto"}})).await;
    let (_, before) = response(&mut reader, 4).await;
    let (prompt, after) = response(&mut reader, 3).await;
    assert_eq!(prompt["result"]["stopReason"], "end_turn", "{prompt}");
    assert!(
        before
            .iter()
            .chain(&after)
            .any(|update| update["params"]["update"]["content"]["text"] == "now auto"),
        "claude never got the new mode: {after:?}"
    );
    child.kill().await.unwrap();
}

#[tokio::test]
async fn a_cancel_sent_right_after_the_prompt_stops_it() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(&bin, "\nsleep 20\n");
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{
            "sessionId":id,"prompt":[{"type":"text","text":"wait"}]
        }}),
    )
    .await;
    send(
        &mut child,
        json!({"jsonrpc":"2.0","method":"session/cancel","params":{"sessionId":id}}),
    )
    .await;
    let (prompt, _) =
        tokio::time::timeout(std::time::Duration::from_secs(10), response(&mut reader, 3))
            .await
            .expect("the prompt ends within 10 s of its cancel");
    assert_eq!(prompt["result"]["stopReason"], "cancelled");
    child.kill().await.unwrap();
}

#[tokio::test]
async fn cancellation_stops_an_active_prompt() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"started"}]}}'
sleep 20
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{
            "sessionId":id,"prompt":[{"type":"text","text":"wait"}]
        }}),
    )
    .await;
    loop {
        let value = receive(&mut reader).await;
        if value["params"]["update"]["content"]["text"] == "started" {
            break;
        }
    }
    send(
        &mut child,
        json!({"jsonrpc":"2.0","method":"session/cancel","params":{"sessionId":id}}),
    )
    .await;
    let (prompt, _) = response(&mut reader, 3).await;
    assert_eq!(prompt["result"]["stopReason"], "cancelled");
    child.kill().await.unwrap();
}

#[tokio::test]
async fn permission_request_round_trips_through_client() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
IFS= read -r initial || exit 10
case "$initial" in *'"type":"user"'*) ;; *) exit 11;; esac
case "$initial" in *'"run touch"'*) ;; *) exit 11;; esac
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"tool-1","name":"Bash","input":{"command":"touch /tmp/claude-approval-test"}}]}}'
printf '%s\n' '{"type":"control_request","request_id":"permission-1","request":{"subtype":"can_use_tool","tool_name":"Bash","tool_use_id":"tool-1","input":{"command":"touch /tmp/claude-approval-test"}}}'
IFS= read -r answer || exit 12
for part in '"type":"control_response"' '"request_id":"permission-1"' '"behavior":"allow"' '"toolUseID":"tool-1"'; do
  case "$answer" in *"$part"*) ;; *) exit 13;; esac
done
printf '%s\n' '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"tool-1","content":"touched"}]}}'
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"approved"}]}}'
printf '%s\n' '{"type":"result","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"done"}'
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{
            "sessionId":id,"prompt":[{"type":"text","text":"run touch"}]
        }}),
    )
    .await;
    let mut before = Vec::new();
    let permission = loop {
        let value = receive(&mut reader).await;
        if value["method"] == "session/request_permission" {
            break value;
        }
        before.push(value);
    };
    assert_eq!(
        before
            .iter()
            .filter(|value| value["params"]["update"]["sessionUpdate"] == "tool_call")
            .count(),
        1
    );
    assert_eq!(permission["params"]["toolCall"]["toolCallId"], "tool-1");
    assert_eq!(
        permission["params"]["toolCall"]["title"],
        "touch /tmp/claude-approval-test"
    );
    assert_eq!(
        permission["params"]["toolCall"]["rawInput"]["command"],
        "touch /tmp/claude-approval-test"
    );
    assert_eq!(permission["params"]["toolCall"]["kind"], "execute");
    assert_eq!(permission["params"]["toolCall"]["status"], "pending");
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":permission["id"],"result":{
            "outcome":{"outcome":"selected","optionId":"allow_once"}
        }}),
    )
    .await;
    let (prompt, updates) = response(&mut reader, 3).await;
    assert_eq!(prompt["result"]["stopReason"], "end_turn");
    assert!(
        updates
            .iter()
            .any(|v| v["params"]["update"]["content"]["text"] == "approved")
    );
    assert!(
        updates
            .iter()
            .any(|v| v["params"]["update"]["status"] == "completed")
    );
    child.kill().await.unwrap();
}

#[tokio::test]
async fn persistent_approval_and_denial_reach_claude() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
IFS= read -r initial || exit 10
printf '%s\n' '{"type":"control_request","request_id":"permission-1","request":{"subtype":"can_use_tool","tool_name":"Bash","tool_use_id":"tool-1","input":{"command":"pwd"},"permission_suggestions":[{"type":"addRules","rules":[{"toolName":"Bash","ruleContent":"pwd"}],"behavior":"allow","destination":"session"}]}}'
IFS= read -r answer || exit 11
case "$answer" in *'"behavior":"allow"'*) ;; *) exit 12;; esac
case "$answer" in *'"updatedPermissions"'*) ;; *) exit 13;; esac
printf '%s\n' '{"type":"control_request","request_id":"permission-2","request":{"subtype":"can_use_tool","tool_name":"Bash","tool_use_id":"tool-2","input":{"command":"whoami"}}}'
IFS= read -r answer || exit 14
case "$answer" in *'"behavior":"deny"'*) ;; *) exit 15;; esac
printf '%s\n' '{"type":"result","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"done"}'
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    send(&mut child, json!({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":id,"prompt":[{"type":"text","text":"test permissions"}]}})).await;
    let first = loop {
        let value = receive(&mut reader).await;
        if value["method"] == "session/request_permission" {
            break value;
        }
    };
    assert!(
        first["params"]["options"]
            .as_array()
            .unwrap()
            .iter()
            .any(|option| option["optionId"] == "allow_always"
                && option["name"].as_str().unwrap().contains("pwd"))
    );
    send(&mut child, json!({"jsonrpc":"2.0","id":first["id"],"result":{"outcome":{"outcome":"selected","optionId":"allow_always"}}})).await;
    let second = loop {
        let value = receive(&mut reader).await;
        if value["method"] == "session/request_permission" {
            break value;
        }
    };
    assert!(
        !second["params"]["options"]
            .as_array()
            .unwrap()
            .iter()
            .any(|option| option["optionId"] == "allow_always")
    );
    send(&mut child, json!({"jsonrpc":"2.0","id":second["id"],"result":{"outcome":{"outcome":"selected","optionId":"reject_once"}}})).await;
    let (prompt, _) = response(&mut reader, 3).await;
    assert_eq!(prompt["result"]["stopReason"], "end_turn", "{prompt}");
    child.kill().await.unwrap();
}

#[tokio::test]
async fn cancellation_stops_a_pending_permission() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
IFS= read -r initial || exit 10
printf '%s\n' '{"type":"control_request","request_id":"permission-1","request":{"subtype":"can_use_tool","tool_name":"Bash","tool_use_id":"tool-1","input":{"command":"pwd"}}}'
IFS= read -r answer
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    send(&mut child, json!({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":id,"prompt":[{"type":"text","text":"run pwd"}]}})).await;
    loop {
        if receive(&mut reader).await["method"] == "session/request_permission" {
            break;
        }
    }
    send(
        &mut child,
        json!({"jsonrpc":"2.0","method":"session/cancel","params":{"sessionId":id}}),
    )
    .await;
    let (prompt, _) = response(&mut reader, 3).await;
    assert_eq!(prompt["result"]["stopReason"], "cancelled");
    child.kill().await.unwrap();
}

#[tokio::test]
async fn passes_stdio_mcp_servers_to_claude() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
found=0
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--mcp-config" ]; then
    grep -q '"command":"node"' "$2" && grep -q '"server.js"' "$2" && grep -q '"TOKEN":"test"' "$2" && found=1
    break
  fi
  shift
done
if [ "$found" -ne 1 ]; then exit 12; fi
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"mcp ok"}]}}'
printf '%s\n' '{"type":"result","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"mcp ok"}'
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{
            "cwd":project,"mcpServers":[{"name":"filesystem","command":"node","args":["server.js"],
                "env":[{"name":"TOKEN","value":"test"}]}]
        }}),
    )
    .await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{
            "sessionId":id,"prompt":[{"type":"text","text":"use mcp"}]
        }}),
    )
    .await;
    let (prompt, updates) = response(&mut reader, 3).await;
    assert_eq!(prompt["result"]["stopReason"], "end_turn");
    assert!(
        updates
            .iter()
            .any(|v| v["params"]["update"]["content"]["text"] == "mcp ok")
    );
    child.kill().await.unwrap();
}

#[tokio::test]
async fn lists_and_resumes_native_claude_history() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    let transcripts = temp.path().join("empty-claude/projects/sample");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fs::create_dir_all(&transcripts).unwrap();
    let native_id = "79ed6f89-b693-4192-8169-4554404b43a2";
    fs::write(transcripts.join(format!("{native_id}.jsonl")), format!(
        "{{\"cwd\":\"{}\",\"type\":\"user\",\"message\":{{\"content\":\"Original prompt\"}}}}\n",
        project.display()
    )).unwrap();
    fake_claude(
        &bin,
        r#"
found=0
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--resume" ] && [ "$2" = "79ed6f89-b693-4192-8169-4554404b43a2" ]; then found=1; break; fi
  shift
done
if [ "$found" -ne 1 ]; then exit 13; fi
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"resumed"}]}}'
printf '%s\n' '{"type":"result","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"resumed"}'
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":2,"method":"session/list","params":{"cwd":project}}),
    )
    .await;
    let (list, _) = response(&mut reader, 2).await;
    assert_eq!(
        list["result"]["sessions"][0]["sessionId"], native_id,
        "{list}"
    );
    assert_eq!(list["result"]["sessions"][0]["title"], "Original prompt");
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":3,"method":"session/resume","params":{
            "sessionId":native_id,"cwd":project,"mcpServers":[]
        }}),
    )
    .await;
    response(&mut reader, 3).await;
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":4,"method":"session/prompt","params":{
            "sessionId":native_id,"prompt":[{"type":"text","text":"continue"}]
        }}),
    )
    .await;
    let (prompt, updates) = response(&mut reader, 4).await;
    assert_eq!(prompt["result"]["stopReason"], "end_turn");
    assert!(
        updates
            .iter()
            .any(|v| v["params"]["update"]["content"]["text"] == "resumed")
    );
    child.kill().await.unwrap();
}

#[tokio::test]
async fn claude_events_become_typed_updates_and_stop_reasons() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        r#"
IFS= read -r initial || exit 10
printf '%s\n' '{"type":"system","subtype":"init","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","slash_commands":["compact","review"],"permissionMode":"default"}'
printf '%s\n' '{"type":"system","subtype":"status","status":"compacting"}'
printf '%s\n' '{"type":"system","subtype":"status","status":null}'
printf '%s\n' '{"type":"assistant","message":{"usage":{"input_tokens":10,"cache_read_input_tokens":4000,"cache_creation_input_tokens":90},"content":[{"type":"tool_use","id":"t1","name":"TodoWrite","input":{"todos":[{"content":"Write the test","status":"completed"},{"content":"Fix it","status":"in_progress"}]}},{"type":"tool_use","id":"t2","name":"Edit","input":{"file_path":"/p/a.cpp","old_string":"old","new_string":"new"}}]}}'
printf '%s\n' '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"},{"type":"tool_result","tool_use_id":"t2","content":[{"type":"text","text":"no such file"}],"is_error":true}]}}'
printf '%s\n' '{"type":"assistant","message":{"stop_reason":"refusal","content":[{"type":"text","text":"I cannot help with that."}]}}'
printf '%s\n' '{"type":"result","subtype":"success","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"","modelUsage":{"m":{"contextWindow":200000}}}'
"#,
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    send(
        &mut child,
        json!({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{
            "sessionId":id,"prompt":[{"type":"text","text":"go"}]
        }}),
    )
    .await;
    let (prompt, updates) = response(&mut reader, 3).await;
    assert_eq!(prompt["result"]["stopReason"], "refusal");
    let of = |kind: &str| -> Vec<&Value> {
        updates
            .iter()
            .map(|v| &v["params"]["update"])
            .filter(|u| u["sessionUpdate"] == kind)
            .collect()
    };
    let commands = of("available_commands_update");
    assert_eq!(commands[0]["availableCommands"][1]["name"], "review");
    let plan = of("plan");
    assert_eq!(plan[0]["entries"][1]["status"], "in_progress");
    let calls = of("tool_call");
    assert_eq!(calls.len(), 1, "TodoWrite is a plan, not a tool call");
    assert_eq!(calls[0]["kind"], "edit");
    assert_eq!(calls[0]["locations"][0]["path"], "/p/a.cpp");
    assert_eq!(calls[0]["content"][0]["type"], "diff");
    assert_eq!(calls[0]["content"][0]["oldText"], "old");
    assert_eq!(calls[0]["content"][0]["newText"], "new");
    let results = of("tool_call_update");
    assert_eq!(results.len(), 1);
    assert_eq!(results[0]["toolCallId"], "t2");
    assert_eq!(results[0]["status"], "failed");
    assert_eq!(results[0]["rawOutput"]["output"], "no such file");
    let usage = of("usage_update");
    assert_eq!(usage[0]["used"], 4100);
    assert_eq!(usage[0]["size"], 200000);
    let compacting: Vec<&Value> = updates
        .iter()
        .filter(|v| v["method"] == "_andromeda/compacting")
        .map(|v| &v["params"]["compacting"])
        .collect();
    assert_eq!(compacting, [&json!(true), &json!(false)]);
    child.kill().await.unwrap();
}

#[tokio::test]
async fn a_run_that_fails_after_output_is_an_error_and_is_not_rerun() {
    let temp = tempfile::tempdir().unwrap();
    let bin = temp.path().join("bin");
    let project = temp.path().join("project");
    let runs = temp.path().join("runs");
    fs::create_dir_all(&bin).unwrap();
    fs::create_dir_all(&project).unwrap();
    fake_claude(
        &bin,
        &format!(
            r#"
IFS= read -r initial || exit 10
echo run >> '{}'
case "$*" in *--resume*) ;; *)
printf '%s\n' '{{"type":"result","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"ok"}}'
exit 0;; esac
printf '%s\n' '{{"type":"assistant","message":{{"content":[{{"type":"tool_use","id":"t1","name":"Bash","input":{{"command":"touch x"}}}}]}}}}'
echo 'boom' >&2
exit 3
"#,
            runs.display()
        ),
    );
    let (mut child, mut reader) = start(&bin, &temp.path().join("store"));
    initialize(&mut child, &mut reader).await;
    send(&mut child, json!({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":project,"mcpServers":[]}})).await;
    let (new, _) = response(&mut reader, 2).await;
    let id = new["result"]["sessionId"].as_str().unwrap().to_owned();
    let mut prompt = async |n: i64| {
        send(
            &mut child,
            json!({"jsonrpc":"2.0","id":n,"method":"session/prompt","params":{
                "sessionId":id,"prompt":[{"type":"text","text":"go"}]
            }}),
        )
        .await;
        response(&mut reader, n).await.0
    };
    let first = prompt(3).await;
    assert_eq!(first["result"]["stopReason"], "end_turn");
    let second = prompt(4).await;
    assert!(
        second["error"]["message"]
            .as_str()
            .unwrap()
            .contains("boom")
    );
    assert_eq!(second["error"]["data"]["exitCode"], 3);
    assert_eq!(second["error"]["data"]["stderr"], "boom");
    assert_eq!(fs::read_to_string(&runs).unwrap().lines().count(), 2);
    child.kill().await.unwrap();
}
