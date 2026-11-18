//! SQLite store for users, their sign-in identities and their tokens.
//!
//! Tokens are random 256-bit strings. Only their SHA-256 hash is stored, so a copy of the
//! database cannot be used to sign in. Kinds:
//!   session  web cookie, 30 days
//!   access   mobile app Bearer token, 15 minutes
//!   refresh  mobile app refresh token, 60 days, replaced on every use
//! An app sign-in creates a "family" id shared by its access and refresh tokens; signing out
//! or reusing an old refresh token revokes the whole family.

use std::{
    sync::Mutex,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use rand::RngCore;
use rusqlite::{params, Connection, OptionalExtension};
use sha2::{Digest, Sha256};

pub const SESSION_LIFETIME: Duration = Duration::from_secs(30 * 24 * 3600);
pub const ACCESS_LIFETIME: Duration = Duration::from_secs(15 * 60);
pub const REFRESH_LIFETIME: Duration = Duration::from_secs(60 * 24 * 3600);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TokenKind {
    Session,
    Access,
    Refresh,
}

impl TokenKind {
    fn as_str(self) -> &'static str {
        match self {
            TokenKind::Session => "session",
            TokenKind::Access => "access",
            TokenKind::Refresh => "refresh",
        }
    }
    fn lifetime(self) -> Duration {
        match self {
            TokenKind::Session => SESSION_LIFETIME,
            TokenKind::Access => ACCESS_LIFETIME,
            TokenKind::Refresh => REFRESH_LIFETIME,
        }
    }
}

#[derive(Clone, Debug)]
pub struct User {
    pub id: i64,
    pub email: String,
    pub name: Option<String>,
}

/// A valid token and the user it belongs to.
#[derive(Clone, Debug)]
pub struct TokenOwner {
    pub user: User,
    pub family: String,
}

/// Result of presenting a refresh token.
#[derive(Debug)]
pub enum Refresh {
    Ok { user: User, family: String, client: String },
    /// An already-used refresh token came back: someone may have copied it. The family is revoked.
    Reused,
    Invalid,
}

pub struct Db {
    conn: Mutex<Connection>,
}

pub fn now() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

/// A new random token, URL-safe, with a short prefix that shows its kind in logs and bug reports.
pub fn new_token(prefix: &str) -> String {
    let mut bytes = [0u8; 32];
    rand::thread_rng().fill_bytes(&mut bytes);
    format!("{prefix}_{}", URL_SAFE_NO_PAD.encode(bytes))
}

fn hash(token: &str) -> Vec<u8> {
    Sha256::digest(token.as_bytes()).to_vec()
}

const SCHEMA: &str = "
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY,
    email         TEXT NOT NULL UNIQUE,
    name          TEXT,
    created_at    INTEGER NOT NULL,
    last_login_at INTEGER
);
CREATE TABLE IF NOT EXISTS identities (
    provider TEXT NOT NULL,
    subject  TEXT NOT NULL,
    user_id  INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    PRIMARY KEY (provider, subject)
);
CREATE TABLE IF NOT EXISTS tokens (
    hash       BLOB PRIMARY KEY,
    kind       TEXT NOT NULL,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    family     TEXT NOT NULL,
    client     TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    used       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS tokens_family ON tokens(family);
";

impl Db {
    pub fn open(path: &str) -> rusqlite::Result<Db> {
        let conn = Connection::open(path)?;
        conn.execute_batch(SCHEMA)?;
        Ok(Db { conn: Mutex::new(conn) })
    }

    fn conn(&self) -> std::sync::MutexGuard<'_, Connection> {
        // A panic while holding the lock leaves the connection itself usable.
        self.conn.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Finds the user for a sign-in identity, linking it to an existing account with the same
    /// email or creating a new account. Updates the stored email/name and last login time.
    pub fn user_for_identity(&self, provider: &str, subject: &str, email: &str, name: Option<&str>) -> rusqlite::Result<User> {
        let email = email.to_lowercase();
        let mut conn = self.conn();
        let tx = conn.transaction()?;
        let by_identity: Option<i64> = tx
            .query_row("SELECT user_id FROM identities WHERE provider = ?1 AND subject = ?2", params![provider, subject], |r| r.get(0))
            .optional()?;
        let user_id = match by_identity {
            Some(id) => {
                // The provider says the email changed: follow it unless another account owns it.
                let taken: Option<i64> =
                    tx.query_row("SELECT id FROM users WHERE email = ?1", params![email], |r| r.get(0)).optional()?;
                if taken.is_none() {
                    tx.execute("UPDATE users SET email = ?1 WHERE id = ?2", params![email, id])?;
                }
                id
            }
            None => {
                let existing: Option<i64> =
                    tx.query_row("SELECT id FROM users WHERE email = ?1", params![email], |r| r.get(0)).optional()?;
                let id = match existing {
                    Some(id) => id,
                    None => {
                        tx.execute("INSERT INTO users (email, created_at) VALUES (?1, ?2)", params![email, now()])?;
                        tx.last_insert_rowid()
                    }
                };
                tx.execute("INSERT INTO identities (provider, subject, user_id) VALUES (?1, ?2, ?3)", params![provider, subject, id])?;
                id
            }
        };
        if let Some(name) = name.filter(|n| !n.trim().is_empty()) {
            tx.execute("UPDATE users SET name = ?1 WHERE id = ?2", params![name.trim(), user_id])?;
        }
        tx.execute("UPDATE users SET last_login_at = ?1 WHERE id = ?2", params![now(), user_id])?;
        let user = tx.query_row("SELECT id, email, name FROM users WHERE id = ?1", params![user_id], |r| {
            Ok(User { id: r.get(0)?, email: r.get(1)?, name: r.get(2)? })
        })?;
        tx.commit()?;
        Ok(user)
    }

    /// Stores a new token and returns it (the only time the plain token exists).
    pub fn issue(&self, kind: TokenKind, user_id: i64, family: &str, client: &str) -> rusqlite::Result<String> {
        let prefix = match kind {
            TokenKind::Session => "ses",
            TokenKind::Access => "at",
            TokenKind::Refresh => "rt",
        };
        let token = new_token(prefix);
        let now = now();
        self.conn().execute(
            "INSERT INTO tokens (hash, kind, user_id, family, client, created_at, expires_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            params![hash(&token), kind.as_str(), user_id, family, client, now, now + kind.lifetime().as_secs() as i64],
        )?;
        Ok(token)
    }

    /// The owner of a valid session or access token.
    pub fn lookup(&self, kind: TokenKind, token: &str) -> Option<TokenOwner> {
        debug_assert!(kind != TokenKind::Refresh, "refresh tokens go through refresh()");
        self.conn()
            .query_row(
                "SELECT u.id, u.email, u.name, t.family FROM tokens t JOIN users u ON u.id = t.user_id
                 WHERE t.hash = ?1 AND t.kind = ?2 AND t.used = 0 AND t.expires_at > ?3",
                params![hash(token), kind.as_str(), now()],
                |r| Ok(TokenOwner { user: User { id: r.get(0)?, email: r.get(1)?, name: r.get(2)? }, family: r.get(3)? }),
            )
            .optional()
            .ok()
            .flatten()
    }

    /// Uses up a refresh token. The caller then issues a new access + refresh pair in the same family.
    pub fn refresh(&self, token: &str) -> rusqlite::Result<Refresh> {
        let mut conn = self.conn();
        let tx = conn.transaction()?;
        let row: Option<(i64, String, i64, bool, String)> = tx
            .query_row(
                "SELECT user_id, family, expires_at, used, client FROM tokens WHERE hash = ?1 AND kind = 'refresh'",
                params![hash(token)],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?)),
            )
            .optional()?;
        let result = match row {
            None => Refresh::Invalid,
            Some((_, family, _, true, _)) => {
                tx.execute("DELETE FROM tokens WHERE family = ?1", params![family])?;
                Refresh::Reused
            }
            Some((_, _, expires, false, _)) if expires <= now() => Refresh::Invalid,
            Some((user_id, family, _, false, client)) => {
                tx.execute("UPDATE tokens SET used = 1 WHERE hash = ?1", params![hash(token)])?;
                // Old access tokens of this family stop working once a new pair is issued.
                tx.execute("DELETE FROM tokens WHERE family = ?1 AND kind = 'access'", params![family])?;
                let user = tx.query_row("SELECT id, email, name FROM users WHERE id = ?1", params![user_id], |r| {
                    Ok(User { id: r.get(0)?, email: r.get(1)?, name: r.get(2)? })
                })?;
                Refresh::Ok { user, family, client }
            }
        };
        tx.commit()?;
        Ok(result)
    }

    /// True while a login still has an unexpired, unused session or refresh token.
    pub fn family_active(&self, family: &str) -> bool {
        self.conn()
            .query_row(
                "SELECT 1 FROM tokens WHERE family = ?1 AND kind IN ('session', 'refresh') AND used = 0 AND expires_at > ?2 LIMIT 1",
                params![family, now()],
                |_| Ok(()),
            )
            .optional()
            .ok()
            .flatten()
            .is_some()
    }

    /// Signs out one login (a web session or an app family).
    pub fn revoke_family(&self, family: &str) -> rusqlite::Result<()> {
        self.conn().execute("DELETE FROM tokens WHERE family = ?1", params![family]).map(|_| ())
    }

    /// Deletes expired tokens. Used refresh tokens are kept until they expire, to detect reuse.
    pub fn prune(&self) -> rusqlite::Result<usize> {
        self.conn().execute("DELETE FROM tokens WHERE expires_at <= ?1", params![now()])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn db() -> Db {
        Db::open(":memory:").unwrap()
    }

    #[test]
    fn identities_link_to_one_account_by_email() {
        let db = db();
        let a = db.user_for_identity("google", "g-1", "Me@Example.com", Some("Me")).unwrap();
        let b = db.user_for_identity("apple", "a-1", "me@example.com", None).unwrap();
        let c = db.user_for_identity("google", "g-1", "me@example.com", None).unwrap();
        assert_eq!(a.id, b.id);
        assert_eq!(a.id, c.id);
        assert_eq!(c.email, "me@example.com");
        assert_eq!(c.name.as_deref(), Some("Me"));
    }

    #[test]
    fn tokens_are_checked_by_kind() {
        let db = db();
        let u = db.user_for_identity("google", "g-1", "me@example.com", None).unwrap();
        let s = db.issue(TokenKind::Session, u.id, "f1", "web").unwrap();
        assert!(db.lookup(TokenKind::Session, &s).is_some());
        assert!(db.lookup(TokenKind::Access, &s).is_none());
        assert!(db.lookup(TokenKind::Session, "ses_nonsense").is_none());
        assert!(db.family_active("f1"));
        db.revoke_family("f1").unwrap();
        assert!(db.lookup(TokenKind::Session, &s).is_none());
        assert!(!db.family_active("f1"));
    }

    #[test]
    fn refresh_rotates_and_detects_reuse() {
        let db = db();
        let u = db.user_for_identity("apple", "a-1", "me@example.com", None).unwrap();
        let access = db.issue(TokenKind::Access, u.id, "fam", "app").unwrap();
        let rt1 = db.issue(TokenKind::Refresh, u.id, "fam", "app").unwrap();

        assert!(matches!(db.refresh(&rt1).unwrap(), Refresh::Ok { .. }));
        assert!(db.lookup(TokenKind::Access, &access).is_none(), "old access token must stop working");
        let rt2 = db.issue(TokenKind::Refresh, u.id, "fam", "app").unwrap();

        // Someone replays the first refresh token: the whole family is signed out.
        assert!(matches!(db.refresh(&rt1).unwrap(), Refresh::Reused));
        assert!(matches!(db.refresh(&rt2).unwrap(), Refresh::Invalid));
        assert!(matches!(db.refresh("rt_unknown").unwrap(), Refresh::Invalid));
    }
}
