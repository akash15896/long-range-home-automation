//! Verifies Google and Apple ID tokens (signed JWTs) and runs Google's code exchange.
//!
//! A token is accepted only if: its signature matches a current key from the provider's JWKS,
//! the issuer is the provider, the audience is one of our client IDs, it has not expired,
//! the email is verified, and the nonce matches when one is expected.

use std::time::{Duration, Instant};

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use jsonwebtoken::{decode, decode_header, jwk::JwkSet, Algorithm, DecodingKey, Validation};
use serde::Deserialize;
use serde_json::Value;
use sha2::{Digest, Sha256};
use tokio::sync::Mutex;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Provider {
    Google,
    Apple,
}

impl Provider {
    pub fn as_str(self) -> &'static str {
        match self {
            Provider::Google => "google",
            Provider::Apple => "apple",
        }
    }
    fn jwks_url(self) -> &'static str {
        match self {
            Provider::Google => "https://www.googleapis.com/oauth2/v3/certs",
            Provider::Apple => "https://appleid.apple.com/auth/keys",
        }
    }
    fn issuers(self) -> &'static [&'static str] {
        match self {
            Provider::Google => &["https://accounts.google.com", "accounts.google.com"],
            Provider::Apple => &["https://appleid.apple.com"],
        }
    }
}

pub const GOOGLE_AUTH_URL: &str = "https://accounts.google.com/o/oauth2/v2/auth";
pub const GOOGLE_TOKEN_URL: &str = "https://oauth2.googleapis.com/token";
pub const APPLE_AUTH_URL: &str = "https://appleid.apple.com/auth/authorize";

/// The parts of a verified ID token we use.
#[derive(Debug, Clone)]
pub struct Identity {
    pub provider: Provider,
    pub subject: String,
    pub email: String,
    pub name: Option<String>,
}

/// How the nonce in a token must be checked.
pub enum NonceCheck<'a> {
    /// Web sign-in: the token's nonce must equal the one we generated.
    Exact(&'a str),
    /// Mobile sign-in: the app sends its raw nonce; the token may hold it as-is or as its
    /// SHA-256 (hex or base64url), as Apple's and Google's native SDKs do. If the app sends
    /// none, the token must not carry one either.
    FromApp(Option<&'a str>),
}

const JWKS_MAX_AGE: Duration = Duration::from_secs(3600);
const JWKS_MIN_REFETCH: Duration = Duration::from_secs(60);

/// Caches one provider's signing keys. Refetches hourly, or sooner when a token names an
/// unknown key (providers rotate keys), but at most once a minute.
pub struct KeyCache {
    provider: Provider,
    keys: Mutex<Option<(Instant, JwkSet)>>,
}

impl KeyCache {
    pub fn new(provider: Provider) -> Self {
        KeyCache { provider, keys: Mutex::new(None) }
    }

    async fn key(&self, http: &reqwest::Client, kid: &str) -> Result<DecodingKey, String> {
        let mut cached = self.keys.lock().await;
        let stale = cached.as_ref().is_none_or(|(at, set)| {
            at.elapsed() > JWKS_MAX_AGE || (set.find(kid).is_none() && at.elapsed() > JWKS_MIN_REFETCH)
        });
        if stale {
            let set: JwkSet = http
                .get(self.provider.jwks_url())
                .send()
                .await
                .and_then(|r| r.error_for_status())
                .map_err(|e| format!("could not fetch {} keys: {e}", self.provider.as_str()))?
                .json()
                .await
                .map_err(|e| format!("bad {} key list: {e}", self.provider.as_str()))?;
            *cached = Some((Instant::now(), set));
        }
        let jwk = cached.as_ref().and_then(|(_, set)| set.find(kid)).ok_or("token signed with an unknown key")?;
        DecodingKey::from_jwk(jwk).map_err(|e| format!("unusable key: {e}"))
    }
}

/// Checks the signature and standard claims, then returns the identity.
pub async fn verify(
    http: &reqwest::Client,
    keys: &KeyCache,
    token: &str,
    audiences: &[String],
    nonce: NonceCheck<'_>,
) -> Result<Identity, String> {
    if audiences.is_empty() {
        return Err("this sign-in method is not set up".into());
    }
    let header = decode_header(token).map_err(|_| "not a valid ID token")?;
    if header.alg != Algorithm::RS256 {
        return Err("unexpected token algorithm".into());
    }
    let kid = header.kid.ok_or("token has no key id")?;
    let key = keys.key(http, &kid).await?;

    let mut validation = Validation::new(Algorithm::RS256);
    validation.set_audience(audiences);
    validation.set_issuer(keys.provider.issuers());
    validation.set_required_spec_claims(&["exp", "iat", "iss", "aud", "sub"]);
    validation.leeway = 60;
    let claims = decode::<Value>(token, &key, &validation).map_err(|e| format!("ID token rejected: {e}"))?.claims;

    identity_from_claims(keys.provider, &claims, nonce)
}

/// Claim checks that don't need the signature (kept separate so they can be unit-tested).
pub fn identity_from_claims(provider: Provider, claims: &Value, nonce: NonceCheck<'_>) -> Result<Identity, String> {
    let token_nonce = claims["nonce"].as_str();
    let nonce_ok = match nonce {
        NonceCheck::Exact(expected) => token_nonce == Some(expected),
        NonceCheck::FromApp(None) => token_nonce.is_none(),
        NonceCheck::FromApp(Some(raw)) => token_nonce.is_some_and(|t| nonce_matches(raw, t)),
    };
    if !nonce_ok {
        return Err("sign-in nonce does not match".into());
    }

    let subject = claims["sub"].as_str().filter(|s| !s.is_empty()).ok_or("token has no subject")?;
    let email = claims["email"].as_str().filter(|e| e.contains('@')).ok_or("the account did not share an email address")?;
    // Google sends a bool, Apple sends "true"/"false" strings (or a bool in newer tokens).
    let verified = matches!(&claims["email_verified"], Value::Bool(true)) || claims["email_verified"] == "true";
    if !verified {
        return Err("the account's email address is not verified".into());
    }
    Ok(Identity {
        provider,
        subject: subject.to_string(),
        email: email.to_lowercase(),
        name: claims["name"].as_str().map(String::from),
    })
}

fn nonce_matches(raw: &str, in_token: &str) -> bool {
    if raw.is_empty() {
        return false;
    }
    let digest = Sha256::digest(raw.as_bytes());
    let hex: String = digest.iter().map(|b| format!("{b:02x}")).collect();
    in_token == raw || in_token.eq_ignore_ascii_case(&hex) || in_token == URL_SAFE_NO_PAD.encode(digest)
}

/// PKCE S256 challenge for a code verifier.
pub fn pkce_challenge(verifier: &str) -> String {
    URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()))
}

#[derive(Deserialize)]
struct GoogleTokenResponse {
    id_token: String,
}

/// Swaps a Google authorization code for an ID token.
pub async fn google_exchange_code(
    http: &reqwest::Client,
    client_id: &str,
    client_secret: &str,
    code: &str,
    redirect_uri: &str,
    code_verifier: &str,
) -> Result<String, String> {
    let form = [
        ("code", code),
        ("client_id", client_id),
        ("client_secret", client_secret),
        ("redirect_uri", redirect_uri),
        ("grant_type", "authorization_code"),
        ("code_verifier", code_verifier),
    ];
    let resp = http.post(GOOGLE_TOKEN_URL).form(&form).send().await.map_err(|e| format!("Google is unreachable: {e}"))?;
    if !resp.status().is_success() {
        let status = resp.status();
        let body = resp.text().await.unwrap_or_default();
        return Err(format!("Google refused the code ({status}): {}", body.chars().take(200).collect::<String>()));
    }
    let tokens: GoogleTokenResponse = resp.json().await.map_err(|e| format!("bad answer from Google: {e}"))?;
    Ok(tokens.id_token)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn claims(nonce: Option<&str>) -> Value {
        let mut c = json!({ "sub": "123", "email": "Me@Example.com", "email_verified": true, "name": "Me" });
        if let Some(n) = nonce {
            c["nonce"] = json!(n);
        }
        c
    }

    #[test]
    fn web_nonce_must_match_exactly() {
        assert!(identity_from_claims(Provider::Google, &claims(Some("abc")), NonceCheck::Exact("abc")).is_ok());
        assert!(identity_from_claims(Provider::Google, &claims(Some("abc")), NonceCheck::Exact("abd")).is_err());
        assert!(identity_from_claims(Provider::Google, &claims(None), NonceCheck::Exact("abc")).is_err());
    }

    #[test]
    fn app_nonce_may_be_hashed() {
        let raw = "raw-nonce";
        let hex: String = Sha256::digest(raw.as_bytes()).iter().map(|b| format!("{b:02x}")).collect();
        let id = identity_from_claims(Provider::Apple, &claims(Some(&hex)), NonceCheck::FromApp(Some(raw))).unwrap();
        assert_eq!(id.email, "me@example.com");
        assert!(identity_from_claims(Provider::Apple, &claims(Some(raw)), NonceCheck::FromApp(Some(raw))).is_ok());
        assert!(identity_from_claims(Provider::Apple, &claims(Some(&hex)), NonceCheck::FromApp(Some("other"))).is_err());
        // A token bound to a nonce cannot be used without it.
        assert!(identity_from_claims(Provider::Apple, &claims(Some(&hex)), NonceCheck::FromApp(None)).is_err());
        assert!(identity_from_claims(Provider::Google, &claims(None), NonceCheck::FromApp(None)).is_ok());
    }

    #[test]
    fn unverified_or_missing_email_is_rejected() {
        let mut c = claims(None);
        c["email_verified"] = json!("false");
        assert!(identity_from_claims(Provider::Apple, &c, NonceCheck::FromApp(None)).is_err());
        c["email_verified"] = json!("true");
        assert!(identity_from_claims(Provider::Apple, &c, NonceCheck::FromApp(None)).is_ok());
        c["email"] = json!("password-login");
        assert!(identity_from_claims(Provider::Apple, &c, NonceCheck::FromApp(None)).is_err());
    }

    #[test]
    fn pkce_matches_rfc7636_example() {
        assert_eq!(pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    }
}
