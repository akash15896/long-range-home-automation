//! Settings read from environment variables (normally /etc/automation/automation.env).
//! See server/deploy/automation.env.example for every variable.

use std::collections::HashSet;

/// What a signed-in person may do.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    /// Switch lights, fans and curtains.
    Member,
    /// Everything, including scan, curtain learn, fan re-home and the master LED.
    Admin,
}

impl Role {
    pub fn as_str(self) -> &'static str {
        match self {
            Role::Member => "member",
            Role::Admin => "admin",
        }
    }
}

/// The account used by the emergency password login. It is always an admin.
pub const PASSWORD_ACCOUNT: &str = "password-login";

pub struct GoogleConfig {
    /// OAuth "Web application" client: used by the web sign-in and accepted as an audience
    /// (Android apps pass it as serverClientId).
    pub web_client_id: String,
    pub web_client_secret: String,
    /// Extra accepted audiences: the iOS / Android OAuth client IDs of the mobile app.
    pub app_client_ids: Vec<String>,
}

pub struct AppleConfig {
    /// Services ID for "Sign in with Apple" on the web (None = web sign-in off).
    pub services_id: Option<String>,
    /// Bundle IDs of the iOS app(s) whose native sign-in tokens are accepted.
    pub bundle_ids: Vec<String>,
}

pub struct Config {
    pub bind: String,
    /// Public address, e.g. https://automation.example.com (no trailing slash).
    pub origin: String,
    pub device_token: String,
    pub db_path: String,
    /// Emergency login password (None = password login off).
    pub password: Option<String>,
    pub admins: HashSet<String>,
    pub members: HashSet<String>,
    pub google: Option<GoogleConfig>,
    pub apple: Option<AppleConfig>,
}

fn var(name: &str) -> Option<String> {
    std::env::var(name).ok().map(|v| v.trim().to_string()).filter(|v| !v.is_empty())
}

fn list(name: &str) -> Vec<String> {
    var(name)
        .map(|v| v.split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect())
        .unwrap_or_default()
}

fn emails(name: &str) -> HashSet<String> {
    list(name).into_iter().map(|e| e.to_lowercase()).collect()
}

impl Config {
    pub fn from_env() -> Self {
        let required = |name: &str| var(name).unwrap_or_else(|| panic!("{name} is not set"));

        let google = match (var("GOOGLE_CLIENT_ID"), var("GOOGLE_CLIENT_SECRET")) {
            (Some(id), Some(secret)) => Some(GoogleConfig {
                web_client_id: id,
                web_client_secret: secret,
                app_client_ids: list("GOOGLE_APP_CLIENT_IDS"),
            }),
            (None, None) if !list("GOOGLE_APP_CLIENT_IDS").is_empty() => {
                panic!("GOOGLE_APP_CLIENT_IDS needs GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET too")
            }
            (None, None) => None,
            _ => panic!("set both GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET, or neither"),
        };

        let apple_services = var("APPLE_SERVICES_ID");
        let apple_bundles = list("APPLE_BUNDLE_IDS");
        let apple = (apple_services.is_some() || !apple_bundles.is_empty())
            .then_some(AppleConfig { services_id: apple_services, bundle_ids: apple_bundles });

        let config = Config {
            bind: var("AUTOMATION_BIND").unwrap_or_else(|| "127.0.0.1:3000".into()),
            origin: required("AUTOMATION_ORIGIN").trim_end_matches('/').to_string(),
            device_token: required("AUTOMATION_DEVICE_TOKEN"),
            db_path: var("AUTOMATION_DB").unwrap_or_else(|| "automation.db".into()),
            password: var("AUTOMATION_PASSWORD"),
            admins: emails("AUTOMATION_ADMINS"),
            members: emails("AUTOMATION_MEMBERS"),
            google,
            apple,
        };

        if config.password.as_deref() == Some("change-me") || config.device_token.starts_with("replace-with") {
            panic!("automation.env still has a placeholder from automation.env.example; set real values");
        }
        if config.password.is_none() && config.google.is_none() && config.apple.is_none() {
            panic!("no way to sign in: set AUTOMATION_PASSWORD, Google or Apple settings");
        }
        if (config.google.is_some() || config.apple.is_some()) && config.admins.is_empty() {
            eprintln!("warning: AUTOMATION_ADMINS is empty, so nobody can sign in with Google/Apple as admin");
        }
        config
    }

    /// The role for an email address, or None if it is not on the allowlist.
    pub fn role_for(&self, email: &str) -> Option<Role> {
        let email = email.to_lowercase();
        if email == PASSWORD_ACCOUNT {
            return self.password.is_some().then_some(Role::Admin);
        }
        if self.admins.contains(&email) {
            Some(Role::Admin)
        } else if self.members.contains(&email) {
            Some(Role::Member)
        } else {
            None
        }
    }
}

#[cfg(test)]
pub fn test_config() -> Config {
    Config {
        bind: "127.0.0.1:0".into(),
        origin: "https://home.example".into(),
        device_token: "dev".into(),
        db_path: ":memory:".into(),
        password: Some("pw".into()),
        admins: ["boss@example.com".to_string()].into(),
        members: ["kid@example.com".to_string()].into(),
        google: None,
        apple: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roles_come_from_the_allowlist() {
        let c = test_config();
        assert_eq!(c.role_for("Boss@Example.com"), Some(Role::Admin));
        assert_eq!(c.role_for("kid@example.com"), Some(Role::Member));
        assert_eq!(c.role_for("stranger@example.com"), None);
        assert_eq!(c.role_for(PASSWORD_ACCOUNT), Some(Role::Admin));
    }

    #[test]
    fn password_account_is_off_without_a_password() {
        let c = Config { password: None, ..test_config() };
        assert_eq!(c.role_for(PASSWORD_ACCOUNT), None);
    }
}
