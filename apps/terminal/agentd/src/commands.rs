use std::{
    fs,
    path::{Path, PathBuf},
};

// Claude Code's own commands: its init message sends their names only.
const BUILT_IN: &[(&str, &str)] = &[
    ("add-dir", "Add a working folder"),
    ("agents", "Manage agent configurations"),
    ("clear", "Clear the conversation history"),
    ("compact", "Summarise the conversation to free context"),
    ("config", "Open Claude settings"),
    ("context", "Show what fills the context window"),
    ("cost", "Show the token use of this session"),
    ("doctor", "Check the Claude Code installation"),
    ("export", "Export the conversation"),
    ("help", "Show help and the available commands"),
    ("hooks", "Manage hook configurations"),
    ("init", "Write a CLAUDE.md file that documents the codebase"),
    ("mcp", "Manage MCP servers"),
    ("memory", "Edit Claude memory files"),
    ("model", "Choose the model"),
    ("permissions", "Manage tool permission rules"),
    ("pr-comments", "Get the comments of a GitHub pull request"),
    ("release-notes", "Show the release notes"),
    ("review", "Review a pull request"),
    (
        "security-review",
        "Review the pending changes on this branch for security issues",
    ),
    (
        "status",
        "Show the version, model, account and connectivity",
    ),
    ("statusline", "Set up the status line"),
    ("todos", "List the current todo items"),
    ("usage", "Show the plan usage limits"),
];

/// The description of slash command `name`: Claude's built-in text, else the `description:` frontmatter (or the
/// first body line) of its command or skill file in `cwd`, `claude_dir` or an installed plugin. Empty when unknown.
pub fn describe(name: &str, cwd: &Path, claude_dir: &Path) -> String {
    if let Some((_, text)) = BUILT_IN.iter().find(|(n, _)| *n == name) {
        return (*text).to_owned();
    }
    candidates(name, cwd, claude_dir)
        .iter()
        .find_map(|path| fs::read_to_string(path).ok())
        .map(|text| from_markdown(&text))
        .unwrap_or_default()
}

fn candidates(name: &str, cwd: &Path, claude_dir: &Path) -> Vec<PathBuf> {
    let nested = name.replace(':', "/");
    let mut out = Vec::new();
    for root in [cwd.join(".claude"), claude_dir.to_path_buf()] {
        out.push(root.join("commands").join(format!("{nested}.md")));
        out.push(root.join("skills").join(name).join("SKILL.md"));
    }
    // Plugin commands read `plugin:command`: plugins/cache/<marketplace>/<plugin>/<version>/...
    if let Some((plugin, command)) = name.split_once(':') {
        for version in subdirs(&claude_dir.join("plugins/cache"))
            .iter()
            .flat_map(|marketplace| subdirs(&marketplace.join(plugin)))
        {
            out.push(version.join("commands").join(format!("{command}.md")));
            out.push(
                version
                    .join(".claude/commands")
                    .join(format!("{command}.md")),
            );
            out.push(version.join("skills").join(command).join("SKILL.md"));
        }
    }
    out
}

fn subdirs(dir: &Path) -> Vec<PathBuf> {
    fs::read_dir(dir)
        .map(|entries| {
            entries
                .flatten()
                .map(|e| e.path())
                .filter(|p| p.is_dir())
                .collect()
        })
        .unwrap_or_default()
}

fn from_markdown(text: &str) -> String {
    let mut body = text;
    if let Some((front, rest)) = text
        .strip_prefix("---")
        .and_then(|rest| rest.split_once("\n---"))
    {
        if let Some(description) = frontmatter_description(&front.lines().collect::<Vec<_>>()) {
            return description;
        }
        body = rest.split_once('\n').map_or("", |(_, after)| after);
    }
    body.lines()
        .map(str::trim)
        .find(|line| !line.is_empty())
        .map(|line| line.trim_start_matches('#').trim().to_owned())
        .unwrap_or_default()
}

fn frontmatter_description(front: &[&str]) -> Option<String> {
    let at = front
        .iter()
        .position(|line| line.starts_with("description:"))?;
    let value = front[at]["description:".len()..].trim();
    let text = if value.is_empty() || value.starts_with('>') || value.starts_with('|') {
        // A block scalar: the indented lines that follow.
        front[at + 1..]
            .iter()
            .take_while(|line| line.starts_with(' ') || line.starts_with('\t') || line.is_empty())
            .map(|line| line.trim())
            .filter(|line| !line.is_empty())
            .collect::<Vec<_>>()
            .join(" ")
    } else {
        value.trim_matches(|c| c == '"' || c == '\'').to_owned()
    };
    (!text.is_empty()).then_some(text)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_inline_block_and_body_descriptions() {
        assert_eq!(
            from_markdown("---\nname: a\ndescription: \"Ship it\"\n---\nbody"),
            "Ship it"
        );
        assert_eq!(
            from_markdown("---\ndescription: >\n  Two\n  lines\nother: x\n---\n"),
            "Two lines"
        );
        assert_eq!(
            from_markdown("---\nname: a\n---\n\n# Fix the build\nmore"),
            "Fix the build"
        );
        assert_eq!(from_markdown("Plain first line\n"), "Plain first line");
    }

    #[test]
    fn finds_project_user_and_plugin_files() {
        let root = std::env::temp_dir().join(format!("agentd-commands-{}", std::process::id()));
        let cwd = root.join("project");
        let claude = root.join("claude");
        let write = |path: PathBuf, text: &str| {
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, text).unwrap();
        };
        write(
            cwd.join(".claude/commands/team/deploy.md"),
            "---\ndescription: Deploy\n---\n",
        );
        write(
            claude.join("skills/ship/SKILL.md"),
            "---\nname: ship\ndescription: Finish a task\n---\n",
        );
        write(
            claude.join("plugins/cache/market/tool/1.0/skills/lint/SKILL.md"),
            "---\ndescription: Lint\n---\n",
        );
        assert_eq!(describe("team:deploy", &cwd, &claude), "Deploy");
        assert_eq!(describe("ship", &cwd, &claude), "Finish a task");
        assert_eq!(describe("tool:lint", &cwd, &claude), "Lint");
        assert_eq!(
            describe("compact", &cwd, &claude),
            "Summarise the conversation to free context"
        );
        assert_eq!(describe("missing", &cwd, &claude), "");
        fs::remove_dir_all(root).unwrap();
    }
}
