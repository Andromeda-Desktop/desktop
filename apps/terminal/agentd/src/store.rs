use std::{
    collections::HashMap,
    fs,
    io::Write,
    path::{Path, PathBuf},
    sync::Mutex,
};

use chrono::Utc;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::runner::McpServer;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Session {
    pub cwd: PathBuf,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub claude_session_id: Option<String>,
    pub created_at: String,
    pub updated_at: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mode: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub model: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub title: Option<String>,
    #[serde(default, rename = "configOverrides")]
    pub config_overrides: HashMap<String, Value>,
    #[serde(skip)]
    pub mcp_servers: Vec<McpServer>,
    #[serde(skip)]
    pub native: bool,
}

impl Session {
    pub fn new(cwd: PathBuf, mcp_servers: Vec<McpServer>) -> Self {
        let now = Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Millis, true);
        Self {
            cwd,
            claude_session_id: None,
            created_at: now.clone(),
            updated_at: now,
            mode: None,
            model: None,
            title: None,
            config_overrides: HashMap::new(),
            mcp_servers,
            native: false,
        }
    }
    pub fn touch(&mut self) {
        self.updated_at = Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Millis, true);
    }
}

pub struct Store {
    dir: PathBuf,
    sessions: Mutex<HashMap<String, Session>>,
}

impl Store {
    pub fn new(dir: PathBuf) -> Self {
        let sessions = fs::read(dir.join("acp-sessions.json"))
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default();
        Self {
            dir,
            sessions: Mutex::new(sessions),
        }
    }

    pub fn get(&self, id: &str) -> Option<Session> {
        self.sessions.lock().unwrap().get(id).cloned()
    }

    pub fn list(&self, cwd: Option<&Path>) -> Vec<(String, Session)> {
        self.sessions
            .lock()
            .unwrap()
            .iter()
            .filter(|(_, session)| cwd.is_none_or(|cwd| cwd == session.cwd))
            .map(|(id, session)| (id.clone(), session.clone()))
            .collect()
    }

    pub fn create(&self, id: String, mut session: Session, restore: bool) {
        if restore {
            let key = persistence_key(&session.cwd);
            session.claude_session_id = self.persisted().get(&key).cloned();
        }
        self.sessions.lock().unwrap().insert(id, session);
        self.save();
    }

    pub fn update(&self, id: &str, update: impl FnOnce(&mut Session)) -> bool {
        let found = {
            let mut sessions = self.sessions.lock().unwrap();
            if let Some(session) = sessions.get_mut(id) {
                update(session);
                session.touch();
                true
            } else {
                false
            }
        };
        if found {
            self.save();
        }
        found
    }

    pub fn set_claude_id(&self, id: &str, claude_id: String, persist: bool) {
        if claude_id.is_empty() {
            return;
        }
        let cwd = self.get(id).map(|s| s.cwd);
        self.update(id, |session| {
            session.claude_session_id = Some(claude_id.clone())
        });
        if persist && let Some(cwd) = cwd {
            let mut saved = self.persisted();
            saved.insert(persistence_key(&cwd), claude_id);
            self.write_json("sessions.json", &saved);
        }
    }

    pub fn clear_persisted(&self, cwd: &Path) {
        let mut saved = self.persisted();
        saved.remove(&persistence_key(cwd));
        self.write_json("sessions.json", &saved);
    }

    fn persisted(&self) -> HashMap<String, String> {
        fs::read(self.dir.join("sessions.json"))
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default()
    }

    fn save(&self) {
        self.write_json("acp-sessions.json", &*self.sessions.lock().unwrap());
    }

    fn write_json<T: Serialize>(&self, name: &str, value: &T) {
        if let Err(err) = (|| -> std::io::Result<()> {
            fs::create_dir_all(&self.dir)?;
            let mut file = tempfile::NamedTempFile::new_in(&self.dir)?;
            serde_json::to_writer_pretty(&mut file, value)?;
            file.flush()?;
            file.persist(self.dir.join(name)).map_err(|e| e.error)?;
            Ok(())
        })() {
            eprintln!("failed to save session registry: {err}");
        }
    }
}

fn persistence_key(cwd: &Path) -> String {
    match std::env::var("ACPX_SESSION_NAME") {
        Ok(name) if !name.is_empty() => format!("session:{name}"),
        _ => format!("cwd:{}", cwd.display()),
    }
}
