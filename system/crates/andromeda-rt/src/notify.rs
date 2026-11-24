//! The system crates' desktop notification client (org.freedesktop.Notifications).

use std::collections::HashMap;

use zbus::{Connection, Variant};

pub const NAME: &str = "org.freedesktop.Notifications";
pub const PATH: &str = "/org/freedesktop/Notifications";

pub struct Notice<'a> {
    pub app: &'a str,
    pub replaces: u32, // 0: none
    pub icon: &'a str,
    pub summary: &'a str,
    pub body: &'a str,
    pub actions: &'a [&'a str], // key, label pairs; "default" is a click on the body
    pub hints: HashMap<&'a str, Variant<'a>>,
}

/// Shows the notice and returns its id; the server picks the expiry.
pub async fn notify(conn: &Connection, notice: &Notice<'_>) -> zbus::Result<u32> {
    let reply = conn
        .call_method(
            Some(NAME),
            PATH,
            Some(NAME),
            "Notify",
            &(
                notice.app,
                notice.replaces,
                notice.icon,
                notice.summary,
                notice.body,
                notice.actions,
                &notice.hints,
                -1i32,
            ),
        )
        .await?;
    reply.body().deserialize::<u32>()
}

pub async fn close(conn: &Connection, id: u32) -> zbus::Result<()> {
    conn.call_method(Some(NAME), PATH, Some(NAME), "CloseNotification", &(id,)).await.map(|_| ())
}
