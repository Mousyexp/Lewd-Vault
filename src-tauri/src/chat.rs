// Lewd Vault Chat: private messages and rooms between Lewd Vault users, with no accounts and no
// server of our own.
//
// Over the internet the messages travel through public Nostr relays (a relay only stores and forwards
// events; anyone can run one). The design is lifted from bitchat (github.com/permissionlesstech/bitchat,
// public domain), tailored and kept apart from it on purpose: our own key derivation and our own room
// kind, so bitchat clients never see or read our traffic and we never see theirs.
//
//   identity      a secp256k1 key made on first use (the "contact code" is its public half)
//   private chat  bitchat's three layers (NostrProtocol.swift): the message (unsigned, kind 14) is
//                 encrypted into a seal signed by the sender (kind 13), which is encrypted again into a
//                 "gift wrap" (kind 1059) made with a throwaway key and a jittered time, tagged only with
//                 the recipient. A relay learns who receives something, never who sent it or what.
//                 Encryption: ECDH on secp256k1 → HKDF-SHA256 → XChaCha20-Poly1305 (24-byte nonce).
//   rooms         "#name" (and an optional password): everyone who knows both derives the same key and
//                 tag, so the relay sees neither the name nor the words. Events of our own kind 4573,
//                 kept 7 days (NIP-40 expiration).
//   pictures      made smaller here (1280 px JPEG), cut into pieces that each travel as a message, and
//                 put back together on arrival. "View once" pictures are erased after being looked at.
//
// Also, as in Signal and WhatsApp: disappearing messages per chat (each message carries its expiry, and
// the relays are asked to drop it then too, NIP-40), every payload padded to a fixed size bucket so its
// length says little, safety numbers to check a contact in person, and pictures that show a blurred
// preview and their progress while their pieces arrive.
//
// Nearby (Bluetooth) carries the very same sealed events phone to phone; the radio part is native
// (mobile/android/LvNearby.kt, mobile/ios/Nearby), this file only wraps and unwraps.
//
// What is stored, in the vault's _app/chat folder (encrypted in Untraceable mode, like everything):
//   state.json   the key, your nickname, contacts, rooms, relays, the outbox
//   c-<id>.json  one conversation's messages        m-<id>.bin  a picture received or sent
// The panic wipe (chatWipe) deletes the folder; the next start makes a new identity.

use base64::Engine;
use chacha20poly1305::aead::{Aead, KeyInit};
use chacha20poly1305::{XChaCha20Poly1305, XNonce};
use k256::schnorr::{Signature, SigningKey, VerifyingKey};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

// the relays: chosen, measured and switched by themselves (src/chat/pool.rs)
mod pool;

type Res<T> = Result<T, String>;

const KIND_SEAL: u32 = 13;
const KIND_RUMOR: u32 = 14;
const KIND_WRAP: u32 = 1059;
/// the same wrap, but ephemeral (NIP-16: relays pass it on and keep nothing): "typing", and the direct
/// link's handshake
const KIND_WRAP_NOW: u32 = 21059;
const KIND_ROOM: u32 = 4573;
const KIND_AUTH: u32 = 22242;
/// the relays someone reads private messages on (NIP-17)
const KIND_INBOX: u32 = 10050;
const MAX_ENVELOPE: usize = 96 * 1024;
const PIECE_DM: usize = 14 * 1024;
const PIECE_ROOM: usize = 24 * 1024;
const MAX_PICTURE: usize = 420 * 1024;
const MAX_VOICE: usize = 900 * 1024;
/// what the direct link takes (it never goes through the relays)
const MAX_FILE: u64 = 2 * 1024 * 1024 * 1024;
const KEEP_MESSAGES: usize = 3000;
const OUTBOX_HOURS: i64 = 24;
const ROOM_DAYS: i64 = 7;
const CODE_PREFIX: &str = "lvchat:";

// ---------- small helpers ----------

fn now() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}
fn now_ms() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}
fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}
fn unhex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok()).collect()
}
fn random<const N: usize>() -> [u8; N] {
    let mut b = [0u8; N];
    getrandom::getrandom(&mut b).expect("no random numbers");
    b
}
fn sha256(b: &[u8]) -> [u8; 32] {
    Sha256::digest(b).into()
}
fn new_id() -> String {
    hex(&random::<8>())
}
fn b64url() -> base64::engine::GeneralPurpose {
    base64::engine::general_purpose::URL_SAFE_NO_PAD
}
fn is_pubkey(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
}
fn clip(s: &str, n: usize) -> String {
    s.chars().take(n).collect()
}
fn clean_nick(s: &str) -> String {
    clip(s.trim(), 24).chars().filter(|c| !c.is_control()).collect()
}
fn new_key() -> SigningKey {
    loop {
        if let Ok(k) = SigningKey::from_bytes(&random::<32>()) {
            return k;
        }
    }
}
fn pub_of(k: &SigningKey) -> String {
    hex(&k.verifying_key().to_bytes())
}

// ---------- Nostr events (NIP-01) ----------

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Event {
    #[serde(default)]
    pub id: String,
    pub pubkey: String,
    pub created_at: i64,
    pub kind: u32,
    pub tags: Vec<Vec<String>>,
    pub content: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sig: Option<String>,
}

impl Event {
    fn new(pubkey: String, created_at: i64, kind: u32, tags: Vec<Vec<String>>, content: String) -> Event {
        Event { id: String::new(), pubkey, created_at, kind, tags, content, sig: None }
    }
    fn hash(&self) -> [u8; 32] {
        sha256(json!([0, self.pubkey, self.created_at, self.kind, self.tags, self.content]).to_string().as_bytes())
    }
    fn sign(mut self, key: &SigningKey) -> Event {
        let h = self.hash();
        let sig = key.sign_raw(&h, &random::<32>()).expect("signing");
        self.id = hex(&h);
        self.sig = Some(hex(&sig.to_bytes()));
        self
    }
    fn verify(&self) -> bool {
        let h = self.hash();
        if hex(&h) != self.id {
            return false;
        }
        let (Some(pk), Some(sig)) = (unhex(&self.pubkey), self.sig.as_deref().and_then(unhex)) else { return false };
        let (Ok(vk), Ok(sig)) = (VerifyingKey::from_bytes(&pk), Signature::try_from(sig.as_slice())) else { return false };
        vk.verify_raw(&h, &sig).is_ok()
    }
    fn tag(&self, name: &str) -> Option<&str> {
        self.tags.iter().find(|t| t.first().map(|s| s.as_str()) == Some(name)).and_then(|t| t.get(1)).map(|s| s.as_str())
    }
    fn json(&self) -> String {
        serde_json::to_string(self).unwrap_or_default()
    }
}

// ---------- encryption ----------

/// The key two identities share: ECDH (the x coordinate, the same whichever way round, and whatever
/// the y parity of the x-only keys) through HKDF with our own label (bitchat uses "nip44-v2").
fn shared_key(mine: &SigningKey, their_pub: &str) -> Res<[u8; 32]> {
    let x = unhex(their_pub).filter(|x| x.len() == 32).ok_or("Bad key")?;
    let mut sec1 = [2u8; 33];
    sec1[1..].copy_from_slice(&x);
    let pk = k256::PublicKey::from_sec1_bytes(&sec1).map_err(|_| "Bad key")?;
    let sk = k256::SecretKey::from_bytes(&mine.to_bytes()).map_err(|_| "Bad key")?;
    let shared = k256::ecdh::diffie_hellman(sk.to_nonzero_scalar(), pk.as_affine());
    let mut k = [0u8; 32];
    hkdf::Hkdf::<Sha256>::new(None, shared.raw_secret_bytes()).expand(b"lewd-vault-chat/v1/dm", &mut k).map_err(|_| "hkdf")?;
    Ok(k)
}

fn lock_box(key: &[u8; 32], plain: &[u8]) -> String {
    let nonce = random::<24>();
    let ct = XChaCha20Poly1305::new(key.into()).encrypt(XNonce::from_slice(&nonce), plain).expect("encrypt");
    let mut all = nonce.to_vec();
    all.extend(ct);
    format!("lv1:{}", b64url().encode(all))
}

fn open_box(key: &[u8; 32], text: &str) -> Res<Vec<u8>> {
    if text.len() > MAX_ENVELOPE * 2 {
        return Err("Too big".into());
    }
    let body = text.strip_prefix("lv1:").ok_or("Not ours")?;
    let all = b64url().decode(body).map_err(|_| "Bad box")?;
    if all.len() < 24 + 16 {
        return Err("Bad box".into());
    }
    XChaCha20Poly1305::new(key.into()).decrypt(XNonce::from_slice(&all[..24]), &all[24..]).map_err(|_| "Can't open".into())
}

/// A time up to 15 minutes back, so a relay can't line up the wraps with when they were sent
/// (bitchat jitters both ways; going forward makes some relays refuse the event).
fn jittered() -> i64 {
    now() - (u16::from_le_bytes(random::<2>()) as i64 % 900)
}

/// A private message, sealed and wrapped for `to` (bitchat's createPrivateMessage).
#[cfg(test)]
pub fn wrap(me: &SigningKey, to: &str, payload: &str) -> Res<Event> {
    wrap_kind(me, to, payload, KIND_WRAP)
}

fn wrap_kind(me: &SigningKey, to: &str, payload: &str, kind: u32) -> Res<Event> {
    wrap_exp(me, to, payload, kind, 0)
}

/// (`exp`: a disappearing message's end, in seconds: the relays are asked to drop it then, NIP-40)
fn wrap_exp(me: &SigningKey, to: &str, payload: &str, kind: u32, exp: i64) -> Res<Event> {
    let my_pub = pub_of(me);
    let mut rumor = Event::new(my_pub.clone(), now(), KIND_RUMOR, vec![], payload.to_string());
    rumor.id = hex(&rumor.hash());
    let seal = Event::new(my_pub, jittered(), KIND_SEAL, vec![], lock_box(&shared_key(me, to)?, rumor.json().as_bytes())).sign(me);
    let once = new_key();
    let wrapped = lock_box(&shared_key(&once, to)?, seal.json().as_bytes());
    let at = if kind == KIND_WRAP { jittered() } else { now() }; // (an ephemeral one is gone at once anyway)
    let mut tags = vec![vec!["p".into(), to.into()]];
    if exp > 0 {
        tags.push(vec!["expiration".into(), exp.to_string()]);
    }
    Ok(Event::new(pub_of(&once), at, kind, tags, wrapped).sign(&once))
}

/// The other way (bitchat's decryptPrivateMessage, with its checks): (sender, time sent, payload).
#[cfg(test)]
pub fn unwrap(me: &SigningKey, ev: &Event) -> Res<(String, i64, String)> {
    unwrap_checked(me, ev, false)
}

/// (`checked`: its id and signature were checked already, as the relay link does for every event)
fn unwrap_checked(me: &SigningKey, ev: &Event, checked: bool) -> Res<(String, i64, String)> {
    let my_pub = pub_of(me);
    if !matches!(ev.kind, KIND_WRAP | KIND_WRAP_NOW) || ev.content.len() > MAX_ENVELOPE * 2 || ev.tags.first() != Some(&vec!["p".to_string(), my_pub.clone()])
        || !ev.tags[1..].iter().all(|t| t.len() == 2 && t[0] == "expiration") || ev.tags.len() > 2 || !(checked || ev.verify()) {
        return Err("Not a wrap for me".into());
    }
    let seal: Event = serde_json::from_slice(&open_box(&shared_key(me, &ev.pubkey)?, &ev.content)?).map_err(|_| "Bad seal")?;
    if seal.kind != KIND_SEAL || !seal.tags.is_empty() || !seal.verify() {
        return Err("Bad seal".into());
    }
    let rumor: Event = serde_json::from_slice(&open_box(&shared_key(me, &seal.pubkey)?, &seal.content)?).map_err(|_| "Bad message")?;
    if rumor.kind != KIND_RUMOR || rumor.sig.is_some() || rumor.pubkey != seal.pubkey {
        return Err("Bad message".into());
    }
    Ok((seal.pubkey, rumor.created_at, rumor.content))
}

/// A payload padded up to the next size bucket (256 bytes, then doubling): what travels says little
/// about how long the message is (Signal and bitchat pad the same way).
fn padded(p: &Value) -> String {
    let s = p.to_string();
    let need = s.len() + 9; // (with `,"pad":""`)
    let mut bucket = 256;
    while bucket < need {
        bucket *= 2;
    }
    let mut q = p.clone();
    q["pad"] = json!("0".repeat(bucket - need));
    q.to_string()
}

/// Safety numbers for two people (the same for both): 12 groups of 5 digits and 8 pictures to compare
/// side by side or read aloud. Equal on both phones: nobody is in between.
pub fn safety(a: &str, b: &str) -> (Vec<String>, Vec<&'static str>) {
    const PICS: [&str; 64] = ["🐶", "🐱", "🦊", "🐻", "🐼", "🐨", "🐯", "🦁", "🐮", "🐷", "🐸", "🐵", "🐔", "🐧", "🐦", "🦉",
        "🦄", "🐝", "🦋", "🐌", "🐞", "🐢", "🐍", "🐙", "🦀", "🐬", "🐳", "🦈", "🐊", "🦒", "🦓", "🐘",
        "🌵", "🌲", "🌴", "🍀", "🍁", "🍄", "🌸", "🌻", "🌙", "⭐", "🔥", "🌈", "⛄", "🍎", "🍋", "🍉",
        "🍇", "🍓", "🍒", "🍑", "🍍", "🥥", "🥕", "🌽", "🍔", "🍕", "🍩", "🍪", "🎂", "🍭", "☕", "🎈"];
    let (lo, hi) = if a < b { (a, b) } else { (b, a) };
    let h = sha256(format!("lewd-vault-chat/safety\n{lo}\n{hi}").as_bytes());
    let mut bytes = sha256(&[&h[..], b"1"].concat()).to_vec();
    bytes.extend(sha256(&[&h[..], b"2"].concat()));
    let digits = bytes.chunks(5).take(12).map(|c| format!("{:05}", c.iter().fold(0u64, |n, x| n * 256 + *x as u64) % 100_000)).collect();
    let pics = h.iter().take(8).map(|x| PICS[(*x as usize) % 64]).collect();
    (digits, pics)
}

/// A room's tag (what the relay filters on) and key, both from its name and password.
fn room_secret(name: &str, password: &str) -> (String, [u8; 32]) {
    let ikm = format!("{}\n{}", name.trim().trim_start_matches('#').to_lowercase(), password);
    let hk = hkdf::Hkdf::<Sha256>::new(Some(b"lewd-vault-chat/v1/room"), ikm.as_bytes());
    let (mut tag, mut key) = ([0u8; 16], [0u8; 32]);
    let _ = hk.expand(b"tag", &mut tag);
    let _ = hk.expand(b"key", &mut key);
    (hex(&tag), key)
}

fn room_event(me: &SigningKey, tag: &str, key: &[u8; 32], payload: &str) -> Event {
    let tags = vec![vec!["r".into(), tag.into()], vec!["expiration".into(), (now() + ROOM_DAYS * 86400).to_string()]];
    Event::new(pub_of(me), now(), KIND_ROOM, tags, lock_box(key, payload.as_bytes())).sign(me)
}

/// A 24 px preview (a few hundred bytes of JPEG, as base64), shown blurred while a picture arrives.
fn tiny_preview(jpeg: &[u8]) -> Option<String> {
    let img = image::load_from_memory(jpeg).ok()?.thumbnail(24, 24).to_rgb8();
    let mut out = Vec::new();
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, 40).encode_image(&img).ok()?;
    Some(base64::engine::general_purpose::STANDARD.encode(out))
}

/// Pictures: no bigger than 1280 px (then 1024, 800) and MAX_PICTURE bytes, as JPEG. (w, h, bytes)
/// (`small`: for Nearby, where Bluetooth is slow, 800 px at most)
pub fn prepare_picture(bytes: &[u8], small: bool) -> Res<(u32, u32, Vec<u8>)> {
    let img = image::load_from_memory(bytes).map_err(|_| "Only pictures can be sent for now.")?;
    let ladder: &[(u32, u8)] = if small { &[(800, 68), (640, 58), (480, 50)] } else { &[(1280, 80), (1024, 72), (800, 65), (640, 55)] };
    // (each step shrinks the last one, not the original: a big photo is decoded and scaled once)
    let mut cur = img;
    for &(side, quality) in ladder {
        if cur.width().max(cur.height()) > side {
            cur = cur.thumbnail(side, side);
        }
        let rgb = cur.to_rgb8();
        let mut out = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, quality).encode_image(&rgb).map_err(|e| e.to_string())?;
        if out.len() <= MAX_PICTURE {
            return Ok((rgb.width(), rgb.height(), out));
        }
    }
    Err("That picture is too big to send.".into())
}

// ---------- what is kept ----------

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Contact {
    nick: String,
    fav: bool,
    blocked: bool,
    added: i64,
    /// they wrote first and you haven't answered or accepted yet
    request: bool,
    /// you compared safety numbers with them
    verified: bool,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Room {
    name: String,
    key: String,
    locked: bool,
    joined: i64,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Meta {
    pinned: bool,
    muted: bool,
    unread: u32,
    last_at: i64,
    last: String,
    hidden: bool,
    /// disappearing messages: how long each lasts, in seconds (0: they stay)
    ttl: i64,
}

#[derive(Serialize, Deserialize, Clone)]
struct Outgoing {
    ev: Event,
    cid: String,
    mid: String,
    at: i64,
}

#[derive(Serialize, Deserialize, Default)]
#[serde(default)]
struct Saved {
    secret: String,
    nick: String,
    relays: Vec<String>,
    contacts: BTreeMap<String, Contact>,
    rooms: BTreeMap<String, Room>,
    meta: BTreeMap<String, Meta>,
    since_dm: i64,
    since_room: i64,
    seen: VecDeque<String>,
    outbox: Vec<Outgoing>,
    /// send read receipts / show that you are typing (both off unless you say so)
    receipts: bool,
    typing: bool,
    /// the relays: what we know of each, whether you use your own list (`relays`) instead, when the
    /// public lists were last fetched, other people's inbox relays, and our own last announcement
    pool: BTreeMap<String, pool::Stat>,
    manual: bool,
    list_at: i64,
    /// (the last time a list actually arrived)
    list_ok: i64,
    inboxes: BTreeMap<String, pool::Inbox>,
    inbox_sig: String,
    inbox_at: i64,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Picture {
    mid: String,
    w: u32,
    h: u32,
    once: bool,
    opened: bool,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Voice {
    mid: String,
    secs: f64,
    mime: String,
}

/// A file offered over the direct link: the card in both chats. `src` (the sender's side only) is the
/// vault item it comes from; `mid` the copy received here, once `done`.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct FileCard {
    fid: String,
    name: String,
    size: u64,
    mime: String,
    kind: String,
    src: String,
    mid: String,
    done: bool,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Loading {
    got: u32,
    n: u32,
    thumb: String,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Msg {
    id: String,
    from: String,
    nick: String,
    at: i64,
    text: String,
    pic: Option<Picture>,
    voice: Option<Voice>,
    file: Option<FileCard>,
    /// (theirs: a read receipt went for it)
    seen: bool,
    reply: String,
    /// a disappearing message's end (ms), 0: it stays
    exp: i64,
    /// a line from the chat itself ("ttl:<seconds>": disappearing messages were set)
    sys: String,
    /// a picture still arriving: pieces so far, of how many, and a tiny blurred preview
    loading: Option<Loading>,
    reacts: BTreeMap<String, Vec<String>>,
    mine: bool,
    /// sending, sent, failed, received
    state: String,
    deleted: bool,
    via: String,
}

struct Partial {
    pieces: Vec<Option<Vec<u8>>>,
    first: Value,
    at: i64,
    started: i64,
}

struct St {
    saved: Saved,
    key: SigningKey,
    me: String,
    msgs: HashMap<String, Vec<Msg>>,
    dirty: HashSet<String>,
    dirty_state: bool,
    parts: HashMap<String, Partial>,
    rate: HashMap<String, VecDeque<i64>>,
    /// the chat on screen, and when the page last asked for it (it counts as open for 5 s after:
    /// an app closed with a chat open mustn't keep it "read" for ever)
    open: String,
    open_at: i64,
    /// someone typing in a chat: when they last said so
    typing: HashMap<String, i64>,
    /// the direct link's handshakes, for the page
    signals: Vec<Value>,
    /// (saved.seen as a set, for a quick "had it already")
    seen: HashSet<String>,
    /// when we last asked for someone's inbox relays (at most every 30 s)
    asked_inbox: HashMap<String, i64>,
}

pub struct Chat {
    dir: PathBuf,
    st: Mutex<St>,
    links: Mutex<Vec<pool::Link>>,
    rev: AtomicU64,
    sub_gen: AtomicU64,
    stopped: AtomicBool,
    pool_on: AtomicBool,
    probing: AtomicBool,
    /// (itself, for the threads it starts from a plain &self)
    me_ref: std::sync::Weak<Chat>,
}

static CHAT: Mutex<Option<Arc<Chat>>> = Mutex::new(None);

fn read_json<T: for<'a> Deserialize<'a> + Default>(p: &Path) -> T {
    crate::crypt::read(p).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

/// The chat, started (relays and all) the first time it is asked for.
pub fn get(dir: &Path) -> Arc<Chat> {
    let mut g = CHAT.lock().unwrap();
    if let Some(c) = g.as_ref() {
        if c.dir == dir {
            return c.clone();
        }
        c.shutdown();
    }
    let c = Chat::open(dir.to_path_buf());
    c.connect();
    Chat::saver(&c);
    *g = Some(c.clone());
    c
}

impl Chat {
    fn open(dir: PathBuf) -> Arc<Chat> {
        let _ = fs::create_dir_all(&dir);
        let mut saved: Saved = read_json(&dir.join("state.json"));
        let key = unhex(&saved.secret).and_then(|b| SigningKey::from_bytes(&b).ok()).unwrap_or_else(|| {
            let k = new_key();
            saved.secret = hex(&k.to_bytes());
            k
        });
        pool::seed(&mut saved.pool);
        let seen: HashSet<String> = saved.seen.iter().cloned().collect();
        if saved.since_dm == 0 {
            saved.since_dm = now() - 86400;
            saved.since_room = now() - ROOM_DAYS * 86400;
        }
        let me = pub_of(&key);
        Arc::new_cyclic(|me_ref| Chat {
            dir,
            st: Mutex::new(St { saved, key, me, msgs: HashMap::new(), dirty: HashSet::new(), dirty_state: true, parts: HashMap::new(), rate: HashMap::new(), open: String::new(), open_at: 0, typing: HashMap::new(), signals: Vec::new(), seen, asked_inbox: HashMap::new() }),
            links: Mutex::new(Vec::new()),
            rev: AtomicU64::new(1),
            sub_gen: AtomicU64::new(1),
            stopped: AtomicBool::new(false),
            pool_on: AtomicBool::new(false),
            probing: AtomicBool::new(false),
            me_ref: me_ref.clone(),
        })
    }

    fn arc(&self) -> Option<Arc<Chat>> {
        self.me_ref.upgrade()
    }

    fn bump(&self) {
        self.rev.fetch_add(1, Ordering::Relaxed);
    }

    fn shutdown(&self) {
        self.stopped.store(true, Ordering::Relaxed);
        for l in self.links.lock().unwrap().drain(..) {
            l.stop.store(true, Ordering::Relaxed);
        }
    }

    /// Writes what changed, at most a few times a second.
    fn saver(me: &Arc<Chat>) {
        let me = me.clone();
        std::thread::spawn(move || {
            let mut n = 0u32;
            loop {
                std::thread::sleep(Duration::from_millis(600));
                if me.stopped.load(Ordering::Relaxed) {
                    return;
                }
                n += 1;
                if n % 5 == 0 {
                    me.sweep();
                }
                me.flush();
            }
        });
    }

    /// Disappearing messages that have ended go, with their files (checked every few seconds; a chat
    /// not loaded yet is checked when it loads).
    fn sweep(&self) {
        let t = now_ms();
        let mut st = self.st.lock().unwrap();
        let mut gone: Vec<String> = Vec::new();
        let mut changed: Vec<String> = Vec::new();
        for (cid, list) in st.msgs.iter_mut() {
            let before = list.len();
            list.retain(|m| {
                let keep = m.exp == 0 || m.exp > t;
                if !keep {
                    gone.extend(m.pic.as_ref().map(|p| p.mid.clone()));
                    gone.extend(m.voice.as_ref().map(|v| v.mid.clone()));
                    gone.extend(m.file.as_ref().map(|f| f.mid.clone()));
                }
                keep
            });
            if list.len() != before {
                changed.push(cid.clone());
            }
        }
        for c in &changed {
            st.dirty.insert(c.clone());
        }
        drop(st);
        for mid in gone.iter().filter(|m| !m.is_empty()) {
            let _ = fs::remove_file(self.media_path(mid));
        }
        if !changed.is_empty() {
            self.bump();
        }
    }

    fn flush(&self) {
        let (state, convs) = {
            let mut st = self.st.lock().unwrap();
            let state = if std::mem::take(&mut st.dirty_state) { serde_json::to_vec(&st.saved).ok() } else { None };
            let convs: Vec<(String, Vec<u8>)> = std::mem::take(&mut st.dirty).into_iter()
                .filter_map(|c| st.msgs.get(&c).and_then(|m| serde_json::to_vec(m).ok()).map(|b| (c, b))).collect();
            (state, convs)
        };
        if self.stopped.load(Ordering::Relaxed) {
            return;
        }
        if let Some(b) = state {
            let _ = crate::write_atomic(&self.dir.join("state.json"), &b);
        }
        for (cid, b) in convs {
            let _ = crate::write_atomic(&self.conv_path(&cid), &b);
        }
    }

    fn conv_path(&self, cid: &str) -> PathBuf {
        self.dir.join(format!("c-{}.json", hex(&sha256(cid.as_bytes())[..10])))
    }
    /// A message's file: m-<id>.<ext> (the extension gives its type to the file server; the first
    /// pictures had none: .bin)
    fn media_path(&self, mid: &str) -> PathBuf {
        let clean: String = mid.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '.').collect();
        let clean = clean.trim_matches('.');
        if clean.contains('.') { self.dir.join(format!("m-{clean}")) } else { self.dir.join(format!("m-{clean}.bin")) }
    }

    // ---------- the subscriptions every relay is asked for ----------

    fn filters(&self) -> Vec<Value> {
        let st = self.st.lock().unwrap();
        // (wraps are back-dated up to 15 minutes, and a phone may be off for a while: two days of margin)
        let mut f = vec![
            json!({ "kinds": [KIND_WRAP], "#p": [st.me], "since": st.saved.since_dm - 2 * 86400 }),
            json!({ "kinds": [KIND_WRAP_NOW], "#p": [st.me], "since": now() - 60 }),
        ];
        if !st.saved.rooms.is_empty() {
            let tags: Vec<&String> = st.saved.rooms.keys().collect();
            f.push(json!({ "kinds": [KIND_ROOM], "#r": tags, "since": st.saved.since_room.max(now() - ROOM_DAYS * 86400) - 600, "limit": 500 }));
        }
        f
    }

    fn outbox_events(&self) -> Vec<String> {
        let mut st = self.st.lock().unwrap();
        let cutoff = now() - OUTBOX_HOURS * 3600;
        let (keep, old): (Vec<_>, Vec<_>) = std::mem::take(&mut st.saved.outbox).into_iter().partition(|o| o.at > cutoff);
        st.saved.outbox = keep;
        for o in old {
            set_state(&mut st, &o.cid, &o.mid, "failed");
        }
        st.saved.outbox.iter().map(|o| json!(["EVENT", o.ev]).to_string()).collect()
    }

    fn publish(&self, cid: &str, mid: &str, events: Vec<Event>) {
        let frames: Vec<String> = events.iter().map(|e| json!(["EVENT", e]).to_string()).collect();
        {
            let mut st = self.st.lock().unwrap();
            for ev in events {
                st.saved.outbox.push(Outgoing { ev, cid: cid.into(), mid: mid.into(), at: now() });
            }
            st.dirty_state = true;
        }
        for f in &frames {
            self.broadcast(f);
        }
        // a private message also goes where they said they read (and we look that up now and then)
        if is_pubkey(cid) {
            if let Some(me) = self.arc() {
                for u in self.inbox_of(cid) {
                    me.deliver(&u, frames.clone());
                }
            }
            self.look_up_inbox(cid);
        }
    }

    fn relay_ok(&self, id: &str) {
        let mut st = self.st.lock().unwrap();
        let Some(pos) = st.saved.outbox.iter().position(|o| o.ev.id == id) else { return };
        let o = st.saved.outbox.remove(pos);
        st.dirty_state = true;
        if !st.saved.outbox.iter().any(|x| x.mid == o.mid && x.cid == o.cid) {
            set_state(&mut st, &o.cid, &o.mid, "sent");
            drop(st);
            self.bump();
        }
    }

    // ---------- what comes in ----------

    /// An event from a relay (or a nearby phone). Returns true if it was something for us.
    pub fn take(&self, ev: Event, via: &str) -> bool {
        self.take_inner(ev, via, false)
    }

    /// The same, for an event whose id and signature were checked already (the relay link's).
    pub(super) fn take_checked(&self, ev: Event, via: &str) -> bool {
        self.take_inner(ev, via, true)
    }

    fn take_inner(&self, ev: Event, via: &str, checked: bool) -> bool {
        if ev.content.len() > MAX_ENVELOPE * 2 || !matches!(ev.kind, KIND_WRAP | KIND_WRAP_NOW | KIND_ROOM) {
            return false;
        }
        let mut st = self.st.lock().unwrap();
        if st.seen.contains(&ev.id) {
            return false;
        }
        let got = match ev.kind {
            KIND_WRAP | KIND_WRAP_NOW => unwrap_checked(&st.key, &ev, checked).ok().map(|(from, at, payload)| {
                if ev.kind == KIND_WRAP {
                    st.saved.since_dm = st.saved.since_dm.max(ev.created_at.min(now()));
                }
                (None, from, at, payload)
            }),
            _ => {
                let tag = ev.tag("r").unwrap_or_default().to_string();
                let key = st.saved.rooms.get(&tag).and_then(|r| unhex(&r.key)).and_then(|k| <[u8; 32]>::try_from(k).ok());
                match key {
                    Some(k) if checked || ev.verify() => open_box(&k, &ev.content).ok().and_then(|b| String::from_utf8(b).ok()).map(|p| {
                        st.saved.since_room = st.saved.since_room.max(ev.created_at.min(now()));
                        (Some(tag), ev.pubkey.clone(), ev.created_at, p)
                    }),
                    _ => None,
                }
            }
        };
        st.saved.seen.push_back(ev.id.clone());
        st.seen.insert(ev.id.clone());
        while st.saved.seen.len() > 6000 {
            if let Some(old) = st.saved.seen.pop_front() {
                st.seen.remove(&old);
            }
        }
        st.dirty_state = true;
        let Some((room, from, at, payload)) = got else { return false };
        let Ok(p) = serde_json::from_str::<Value>(&payload) else { return false };
        let changed = self.handle(&mut st, room, &from, at, &p, via);
        drop(st);
        if changed {
            self.bump();
        }
        changed
    }

    fn handle(&self, st: &mut St, room: Option<String>, from: &str, at: i64, p: &Value, via: &str) -> bool {
        let me = st.me.clone();
        let mine = from == me;
        // a copy of what you sent, from another device with the same vault: it says who it was for
        let cid = match &room {
            Some(tag) => format!("r:{tag}"),
            None if mine => match p["to"].as_str() { Some(to) if is_pubkey(to) => to.to_string(), _ => return false },
            None => from.to_string(),
        };
        if !mine && st.saved.contacts.get(from).map(|c| c.blocked).unwrap_or(false) {
            return false;
        }
        load_conv(self, st, &cid);
        // rooms: at most 20 messages a minute from one key (a flood is dropped)
        if room.is_some() && !mine {
            let q = st.rate.entry(from.to_string()).or_default();
            let t = now();
            while q.front().map(|x| t - x > 60).unwrap_or(false) {
                q.pop_front();
            }
            if q.len() >= 20 && p["t"] != "img" {
                return false;
            }
            q.push_back(t);
        }
        let nick = clean_nick(p["nick"].as_str().unwrap_or(""));
        if room.is_none() && !mine {
            let c = st.saved.contacts.entry(from.to_string()).or_insert_with(|| Contact { added: now(), request: true, ..Default::default() });
            if !nick.is_empty() && c.nick != nick {
                c.nick = nick.clone();
            }
        }
        let id = clip(p["id"].as_str().unwrap_or(""), 32);
        let at_ms = p["at"].as_i64().unwrap_or(at * 1000).min(now_ms());
        // (a disappearing message that has already ended: not shown at all)
        if expiry_of(p) != 0 && expiry_of(p) <= now_ms() {
            return false;
        }
        match p["t"].as_str().unwrap_or("") {
            "msg" | "hi" => {
                if id.is_empty() {
                    return false;
                }
                let text = clip(p["text"].as_str().unwrap_or(""), 4000);
                st.typing.remove(&cid);
                if p["t"] == "hi" && text.is_empty() {
                    return room.is_none() && !mine; // (a hello: the contact appears, nothing to show)
                }
                let reply = clip(p["reply"].as_str().unwrap_or(""), 32);
                add_msg(st, &cid, Msg { id, from: from.into(), nick, at: at_ms, exp: expiry_of(p), text, reply, mine, state: if mine { "sent".into() } else { "received".into() }, via: via.into(), ..Default::default() })
            }
            "img" | "voice" => {
                let voice = p["t"] == "voice";
                let (n, i) = (p["n"].as_u64().unwrap_or(0) as usize, p["i"].as_u64().unwrap_or(0) as usize);
                if id.is_empty() || n == 0 || n > 64 || i >= n {
                    return false;
                }
                // (already here whole: nothing to do; a picture still arriving shows its progress)
                self.known(st, &cid, &id);
                let arriving = match st.msgs.get(&cid).and_then(|m| m.iter().find(|x| x.id == id)) {
                    Some(m) if m.loading.is_none() => return false,
                    Some(_) => true,
                    None => false,
                };
                let Ok(piece) = base64::engine::general_purpose::STANDARD.decode(p["d"].as_str().unwrap_or("")) else { return false };
                let key = format!("{cid}/{from}/{id}");
                let part = st.parts.entry(key.clone()).or_insert_with(|| Partial { pieces: vec![None; n], first: Value::Null, at: at_ms, started: now() });
                if part.pieces.len() != n {
                    return false;
                }
                part.pieces[i] = Some(piece);
                if i == 0 {
                    part.first = p.clone(); // (the first piece says what it is: size, preview, text)
                }
                let got = part.pieces.iter().filter(|x| x.is_some()).count() as u32;
                let first = part.first.clone();
                if got < n as u32 {
                    // (pieces that never complete are given up after an hour)
                    st.parts.retain(|_, v| now() - v.started < 3600);
                    if voice || first.is_null() {
                        return false;
                    }
                    // a picture shows at once: its blurred preview, and how much has come
                    if arriving {
                        if let Some(l) = st.msgs.get_mut(&cid).and_then(|m| m.iter_mut().find(|x| x.id == id)).and_then(|m| m.loading.as_mut()) {
                            l.got = got;
                        }
                        return true;
                    }
                    let thumb = first["th"].as_str().filter(|t| t.len() < 4000).map(|t| format!("data:image/jpeg;base64,{t}")).unwrap_or_default();
                    let pic = Picture { mid: String::new(), w: first["w"].as_u64().unwrap_or(0) as u32, h: first["h"].as_u64().unwrap_or(0) as u32, once: first["once"].as_bool().unwrap_or(false), opened: false };
                    let exp = expiry_of(&first);
                    return add_msg(st, &cid, Msg { id, from: from.into(), nick, at: at_ms, exp, pic: Some(pic), loading: Some(Loading { got, n: n as u32, thumb }), mine, state: "received".into(), via: via.into(), ..Default::default() });
                }
                let part = st.parts.remove(&key).unwrap();
                let bytes: Vec<u8> = part.pieces.into_iter().flatten().flatten().collect();
                let f = &part.first;
                let exp = expiry_of(f);
                st.typing.remove(&cid);
                if voice {
                    let mime = clip(f["mime"].as_str().unwrap_or(""), 40);
                    if bytes.len() > MAX_VOICE || !mime.starts_with("audio/") {
                        return false;
                    }
                    let mid = format!("{}.{}", new_id(), ext_for(&mime));
                    if crate::write_atomic(&self.media_path(&mid), &bytes).is_err() {
                        return false;
                    }
                    let v = Voice { mid, secs: f["secs"].as_f64().unwrap_or(0.0).clamp(0.0, 600.0), mime };
                    return add_msg(st, &cid, Msg { id, from: from.into(), nick, at: part.at, exp, voice: Some(v), mine, state: if mine { "sent".into() } else { "received".into() }, via: via.into(), ..Default::default() });
                }
                let Ok(format) = image::guess_format(&bytes) else { return false };
                if bytes.len() > MAX_PICTURE * 2 {
                    return false;
                }
                let mid = format!("{}.{}", new_id(), format.extensions_str().first().copied().unwrap_or("jpg"));
                if crate::write_atomic(&self.media_path(&mid), &bytes).is_err() {
                    return false;
                }
                let pic = Picture { mid, w: f["w"].as_u64().unwrap_or(0) as u32, h: f["h"].as_u64().unwrap_or(0) as u32, once: f["once"].as_bool().unwrap_or(false), opened: false };
                let text = clip(f["text"].as_str().unwrap_or(""), 1000);
                // (the preview that was showing becomes the picture)
                if let Some(m) = st.msgs.get_mut(&cid).and_then(|m| m.iter_mut().find(|x| x.id == id)) {
                    m.pic = Some(pic);
                    m.text = text;
                    m.loading = None;
                    st.dirty.insert(cid);
                    return true;
                }
                add_msg(st, &cid, Msg { id, from: from.into(), nick, at: part.at, exp, text, pic: Some(pic), mine, state: if mine { "sent".into() } else { "received".into() }, via: via.into(), ..Default::default() })
            }
            // disappearing messages were set (by them, or by you on another device)
            "ttl" => {
                if room.is_some() || id.is_empty() {
                    return false;
                }
                let ttl = p["ttl"].as_i64().unwrap_or(0).clamp(0, 30 * 86400);
                st.saved.meta.entry(cid.clone()).or_default().ttl = ttl;
                st.dirty_state = true;
                add_msg(st, &cid, Msg { id, from: from.into(), nick, at: at_ms, sys: format!("ttl:{ttl}"), mine, state: "received".into(), ..Default::default() })
            }
            // someone typing (private chats only; ephemeral)
            "typing" => {
                if room.is_some() || mine {
                    return false;
                }
                st.typing.insert(cid, now_ms());
                true
            }
            // a read receipt: your messages they have seen
            "read" => {
                if room.is_some() || mine {
                    return false;
                }
                let refs: HashSet<&str> = p["refs"].as_array().map(|a| a.iter().filter_map(|v| v.as_str()).collect()).unwrap_or_default();
                let mut changed = false;
                if let Some(list) = st.msgs.get_mut(&cid) {
                    for m in list.iter_mut().filter(|m| m.mine && refs.contains(m.id.as_str()) && m.state != "read") {
                        m.state = "read".into();
                        changed = true;
                    }
                }
                if changed {
                    st.dirty.insert(cid);
                }
                changed
            }
            // a file offered over the direct link (the bytes come later, device to device)
            "file" => {
                if room.is_some() || mine || id.is_empty() {
                    return false;
                }
                let size = p["size"].as_u64().unwrap_or(0);
                if size == 0 || size > MAX_FILE {
                    return false;
                }
                let card = FileCard { fid: id.clone(), name: clip(p["name"].as_str().unwrap_or("file"), 120), size, mime: clip(p["mime"].as_str().unwrap_or(""), 60),
                                      kind: clip(p["kind"].as_str().unwrap_or("file"), 12), ..Default::default() };
                st.typing.remove(&cid);
                add_msg(st, &cid, Msg { id, from: from.into(), nick, at: at_ms, exp: expiry_of(p), file: Some(card), state: "received".into(), via: via.into(), ..Default::default() })
            }
            // the direct link's handshake: for the page, which makes the link (WebRTC)
            "rtc" => {
                if room.is_some() || mine {
                    return false;
                }
                st.signals.push(json!({ "cid": cid, "fid": clip(p["fid"].as_str().unwrap_or(""), 32), "sig": p["sig"] }));
                if st.signals.len() > 60 {
                    st.signals.remove(0);
                }
                true
            }
            "react" => {
                let (r, e) = (p["ref"].as_str().unwrap_or(""), clip(p["e"].as_str().unwrap_or(""), 8));
                let on = p["on"].as_bool().unwrap_or(true);
                let Some(m) = st.msgs.get_mut(&cid).and_then(|l| l.iter_mut().find(|m| m.id == r)) else { return false };
                let who = m.reacts.entry(e.clone()).or_default();
                who.retain(|x| x != from);
                if on {
                    who.push(from.to_string());
                }
                m.reacts.retain(|_, v| !v.is_empty());
                st.dirty.insert(cid);
                true
            }
            "del" => {
                let r = p["ref"].as_str().unwrap_or("");
                let Some(m) = st.msgs.get_mut(&cid).and_then(|l| l.iter_mut().find(|m| m.id == r && m.from == from)) else { return false };
                m.deleted = true;
                m.text.clear();
                if let Some(pic) = m.pic.take() {
                    let _ = fs::remove_file(self.media_path(&pic.mid));
                }
                st.dirty.insert(cid);
                true
            }
            _ => false,
        }
    }

    fn known(&self, st: &mut St, cid: &str, id: &str) -> bool {
        load_conv(self, st, cid);
        st.msgs.get(cid).map(|m| m.iter().any(|x| x.id == id)).unwrap_or(false)
    }

    // ---------- what goes out ----------

    fn payload(&self, st: &St, t: &str, extra: Value) -> Value {
        let mut p = json!({ "v": 1, "t": t, "nick": st.saved.nick, "at": now_ms() });
        // (in a chat with disappearing messages, each message carries its end)
        if matches!(t, "msg" | "img" | "voice" | "file") {
            if let Some(cid) = extra["cid"].as_str() {
                let ttl = st.saved.meta.get(cid).map(|m| m.ttl).unwrap_or(0);
                if ttl > 0 {
                    p["exp"] = json!(now_ms() + ttl * 1000);
                }
            }
        }
        if let (Some(o), Some(e)) = (p.as_object_mut(), extra.as_object()) {
            for (k, v) in e {
                if k != "cid" {
                    o.insert(k.clone(), v.clone());
                }
            }
        }
        p
    }

    /// Seals `payloads` for a conversation: a person (and a copy for your other devices) or a room.
    fn seal_for(&self, st: &St, cid: &str, payloads: &[Value], copy: bool) -> Res<Vec<Event>> {
        let mut out = Vec::new();
        if let Some(tag) = cid.strip_prefix("r:") {
            let room = st.saved.rooms.get(tag).ok_or("You left that room.")?;
            let key: [u8; 32] = unhex(&room.key).and_then(|k| k.try_into().ok()).ok_or("Bad room")?;
            for p in payloads {
                out.push(room_event(&st.key, tag, &key, &padded(p)));
            }
        } else {
            if !is_pubkey(cid) {
                return Err("No such chat".into());
            }
            for p in payloads {
                // (a disappearing message: the relays are asked to drop it when it ends, NIP-40)
                let exp = p["exp"].as_i64().map(|ms| ms / 1000 + 60).unwrap_or(0);
                out.push(wrap_exp(&st.key, cid, &padded(p), KIND_WRAP, exp)?);
                if copy {
                    let mut c = p.clone();
                    c["to"] = json!(cid);
                    out.push(wrap_exp(&st.key, &st.me, &padded(&c), KIND_WRAP, exp)?);
                }
            }
        }
        Ok(out)
    }

    pub fn send_text(&self, cid: &str, text: &str, reply: &str) -> Res<Value> {
        let text = clip(text.trim(), 4000);
        if text.is_empty() {
            return Err("Nothing to send".into());
        }
        let id = new_id();
        let events = {
            let mut st = self.st.lock().unwrap();
            let p = self.payload(&st, "msg", json!({ "id": id, "text": text, "reply": reply, "cid": cid }));
            let events = self.seal_for(&st, cid, &[p], true)?;
            let (me, nick) = (st.me.clone(), st.saved.nick.clone());
            accept(&mut st, cid);
            add_msg(&mut st, cid, Msg { id: id.clone(), from: me, nick, at: now_ms(), text, reply: clip(reply, 32), mine: true, state: "sending".into(), via: "net".into(), ..Default::default() });
            events
        };
        self.publish(cid, &id, events);
        self.bump();
        Ok(json!(id))
    }

    pub fn send_picture(&self, cid: &str, bytes: &[u8], once: bool, text: &str) -> Res<Value> {
        self.send_picture_sized(cid, bytes, once, text, false)
    }

    fn send_picture_sized(&self, cid: &str, bytes: &[u8], once: bool, text: &str, small: bool) -> Res<Value> {
        let (w, h, jpeg) = prepare_picture(bytes, small)?;
        let mid = format!("{}.jpg", new_id());
        // a tiny preview rides on the first piece: it shows, blurred, while the rest arrives (not for
        // a view-once picture)
        let th = if once { String::new() } else { tiny_preview(&jpeg).unwrap_or_default() };
        let first = json!({ "w": w, "h": h, "once": once, "text": clip(text, 1000), "th": th });
        let pic = Picture { mid: mid.clone(), w, h, once, opened: false };
        self.send_pieces(cid, "img", &jpeg, &mid, first, Msg { text: clip(text, 1000), pic: Some(pic), ..Default::default() })
    }

    /// A voice note (the page records it: WebM/Opus, or MP4/AAC on iPhones).
    pub fn send_voice(&self, cid: &str, bytes: &[u8], mime: &str, secs: f64) -> Res<Value> {
        let mime = clip(mime.split(';').next().unwrap_or("").trim(), 40);
        if !mime.starts_with("audio/") {
            return Err("That isn't a recording.".into());
        }
        if bytes.is_empty() || bytes.len() > MAX_VOICE {
            return Err("That voice note is too long (about two minutes at most).".into());
        }
        let mid = format!("{}.{}", new_id(), ext_for(&mime));
        let secs = (secs * 10.0).round() / 10.0;
        let first = json!({ "mime": mime, "secs": secs });
        let voice = Voice { mid: mid.clone(), secs, mime };
        self.send_pieces(cid, "voice", bytes, &mid, first, Msg { voice: Some(voice), ..Default::default() })
    }

    /// Something big for the relays (a picture, a voice note): kept here as `mid`, cut into pieces that
    /// each travel as a message (`first` rides on the first), and shown at once as `msg`.
    fn send_pieces(&self, cid: &str, t: &str, bytes: &[u8], mid: &str, first: Value, msg: Msg) -> Res<Value> {
        crate::write_atomic(&self.media_path(mid), bytes)?;
        let id = new_id();
        let size = if cid.starts_with("r:") { PIECE_ROOM } else { PIECE_DM };
        let pieces: Vec<&[u8]> = bytes.chunks(size).collect();
        let n = pieces.len();
        let events = {
            let mut st = self.st.lock().unwrap();
            let payloads: Vec<Value> = pieces.iter().enumerate().map(|(i, d)| {
                let mut extra = json!({ "id": id, "n": n, "i": i, "d": base64::engine::general_purpose::STANDARD.encode(d) });
                if i == 0 {
                    if let (Some(o), Some(f)) = (extra.as_object_mut(), first.as_object()) {
                        for (k, v) in f {
                            o.insert(k.clone(), v.clone());
                        }
                    }
                    extra["cid"] = json!(cid);
                }
                self.payload(&st, t, extra)
            }).collect();
            // (no copy for your other devices: it is big, and in a room theirs comes anyway)
            let events = self.seal_for(&st, cid, &payloads, false)?;
            let (me, nick) = (st.me.clone(), st.saved.nick.clone());
            accept(&mut st, cid);
            add_msg(&mut st, cid, Msg { id: id.clone(), from: me, nick, at: now_ms(), mine: true, state: "sending".into(), via: "net".into(), ..msg });
            events
        };
        self.publish(cid, &id, events);
        self.bump();
        Ok(json!(id))
    }

    /// Something only worth sending now (typing, the direct link's handshake): an ephemeral wrap to
    /// the relays connected right now, no outbox, no copy.
    fn send_now(&self, cid: &str, t: &str, extra: Value) -> Res<()> {
        if !is_pubkey(cid) {
            return Err("Only in a private chat".into());
        }
        let frame = {
            let st = self.st.lock().unwrap();
            let p = self.payload(&st, t, extra);
            json!(["EVENT", wrap_kind(&st.key, cid, &p.to_string(), KIND_WRAP_NOW)?]).to_string()
        };
        self.broadcast(&frame);
        Ok(())
    }

    /// Read receipts (when you allow them): their messages you have now seen.
    fn send_receipts(&self, cid: &str) {
        let refs: Vec<String> = {
            let mut st = self.st.lock().unwrap();
            if !st.saved.receipts || !is_pubkey(cid) {
                return;
            }
            let Some(list) = st.msgs.get_mut(cid) else { return };
            let refs: Vec<String> = list.iter_mut().filter(|m| !m.mine && !m.seen && !m.deleted).map(|m| { m.seen = true; m.id.clone() }).collect();
            if !refs.is_empty() {
                st.dirty.insert(cid.to_string());
            }
            refs
        };
        if !refs.is_empty() {
            let _ = self.send_quiet(cid, "read", json!({ "refs": refs }));
        }
    }

    /// A small stored message with no copy for your other devices (a read receipt).
    fn send_quiet(&self, cid: &str, t: &str, extra: Value) -> Res<()> {
        let events = {
            let st = self.st.lock().unwrap();
            let p = self.payload(&st, t, extra);
            self.seal_for(&st, cid, &[p], false)?
        };
        self.publish(cid, "", events);
        Ok(())
    }

    /// A file for the direct link: the card for both of you; the bytes go device to device later.
    fn offer_file(&self, cid: &str, f: &Value) -> Res<Value> {
        if !is_pubkey(cid) {
            return Err("Big files go to one person: open a private chat.".into());
        }
        let size = f["size"].as_u64().unwrap_or(0);
        if size == 0 || size > MAX_FILE {
            return Err("That file is too big (2 GB at most).".into());
        }
        let id = new_id();
        let card = FileCard { fid: id.clone(), name: clip(f["name"].as_str().unwrap_or("file"), 120), size, mime: clip(f["mime"].as_str().unwrap_or(""), 60),
                              kind: clip(f["kind"].as_str().unwrap_or("file"), 12), src: clip(f["src"].as_str().unwrap_or(""), 200), ..Default::default() };
        let events = {
            let mut st = self.st.lock().unwrap();
            let p = self.payload(&st, "file", json!({ "id": id, "name": card.name, "size": size, "mime": card.mime, "kind": card.kind, "cid": cid }));
            let events = self.seal_for(&st, cid, &[p], false)?;
            let (me, nick) = (st.me.clone(), st.saved.nick.clone());
            accept(&mut st, cid);
            add_msg(&mut st, cid, Msg { id: id.clone(), from: me, nick, at: now_ms(), file: Some(card), mine: true, state: "sending".into(), via: "net".into(), ..Default::default() });
            events
        };
        self.publish(cid, &id, events);
        self.bump();
        Ok(json!(id))
    }

    /// A piece of a file arriving over the direct link, written to its place here.
    fn file_part(&self, cid: &str, fid: &str, data: &[u8], first: bool) -> Res<Value> {
        let path = {
            let st = self.st.lock().unwrap();
            let card = st.msgs.get(cid).and_then(|l| l.iter().find(|m| m.id == fid)).and_then(|m| m.file.clone()).ok_or("No such file")?;
            self.media_path(&format!("{fid}.{}", ext_for(&card.mime)))
        };
        crate::crypt::write_piece(&path, first, None, &mut &data[..]).map_err(|e| e.to_string())?;
        Ok(json!(true))
    }

    fn file_done(&self, cid: &str, fid: &str) -> Res<Value> {
        let mut st = self.st.lock().unwrap();
        let m = st.msgs.get_mut(cid).and_then(|l| l.iter_mut().find(|m| m.id == fid)).ok_or("No such file")?;
        let card = m.file.as_mut().ok_or("No such file")?;
        let mid = format!("{fid}.{}", ext_for(&card.mime));
        let got = std::fs::metadata(self.media_path(&mid)).map(|x| x.len()).unwrap_or(0);
        // (an encrypted file has a 16-byte head)
        if got < card.size {
            return Err("The file didn't arrive whole.".into());
        }
        card.mid = mid;
        card.done = true;
        st.dirty.insert(cid.to_string());
        drop(st);
        self.bump();
        Ok(json!(true))
    }

    fn send_small(&self, cid: &str, t: &str, extra: Value) -> Res<()> {
        let events = {
            let st = self.st.lock().unwrap();
            let p = self.payload(&st, t, extra);
            self.seal_for(&st, cid, &[p], true)?
        };
        self.publish(cid, "", events);
        Ok(())
    }

    // ---------- for the page ----------

    fn summary(&self) -> Value {
        // (the links before the state: the two locks are never held together)
        let relays = self.links_view();
        let st = self.st.lock().unwrap();
        let t = now();
        let pool = json!({
            "mode": if Chat::manual_relays(&st.saved).is_some() { "manual" } else { "auto" },
            "yours": if st.saved.manual { st.saved.relays.clone() } else { Vec::new() },
            "known": st.saved.pool.len(),
            "working": st.saved.pool.values().filter(|s| s.working(t)).count(),
            "listAt": st.saved.list_ok,
        });
        let mut convs = Vec::new();
        let cids: Vec<String> = st.saved.contacts.iter().filter(|(_, c)| !c.blocked).map(|(k, _)| k.clone())
            .chain(st.saved.rooms.keys().map(|t| format!("r:{t}"))).collect();
        for cid in cids {
            let meta = st.saved.meta.get(&cid).cloned().unwrap_or_default();
            if meta.hidden {
                continue;
            }
            let (name, kind, extra) = match cid.strip_prefix("r:") {
                Some(tag) => {
                    let r = &st.saved.rooms[tag];
                    (format!("#{}", r.name), "room", json!({ "locked": r.locked }))
                }
                None => {
                    let c = &st.saved.contacts[&cid];
                    (if c.nick.is_empty() { format!("Someone {}", &cid[..6]) } else { c.nick.clone() }, "dm", json!({ "fav": c.fav, "request": c.request, "verified": c.verified }))
                }
            };
            let typing = st.typing.get(&cid).copied().unwrap_or(0);
            convs.push(json!({ "id": cid, "kind": kind, "name": name, "pinned": meta.pinned, "muted": meta.muted, "unread": meta.unread,
                               "at": meta.last_at, "last": meta.last, "extra": extra, "typing": typing, "ttl": meta.ttl }));
        }
        let blocked: Vec<Value> = st.saved.contacts.iter().filter(|(_, c)| c.blocked).map(|(k, c)| json!({ "id": k, "nick": c.nick })).collect();
        json!({
            "rev": self.rev.load(Ordering::Relaxed),
            "me": { "id": st.me, "nick": st.saved.nick, "code": format!("{CODE_PREFIX}{}", st.me) },
            "relays": relays,
            "custom": std::env::var("LV_CHAT_RELAYS").is_ok(),
            "pool": pool,
            "convs": convs,
            "blocked": blocked,
            "outbox": st.saved.outbox.len(),
            "prefs": { "receipts": st.saved.receipts, "typing": st.saved.typing },
            "signals": st.signals.len(),
            "now": now_ms(),
        })
    }

    /// A chat's newest `limit` messages (the page asks for more as you scroll up).
    fn messages(&self, cid: &str, limit: usize) -> Value {
        let mut st = self.st.lock().unwrap();
        load_conv(self, &mut st, cid);
        st.open = cid.to_string();
        st.open_at = now_ms();
        if let Some(m) = st.saved.meta.get_mut(cid) {
            if m.unread > 0 {
                m.unread = 0;
                st.dirty_state = true;
            }
        }
        let list = st.msgs.get(cid).map(|l| &l[l.len().saturating_sub(limit)..]).unwrap_or(&[]);
        json!(list)
    }

    fn react(&self, cid: &str, mid: &str, e: &str) -> Res<Value> {
        let e = clip(e, 8);
        let on = {
            let mut st = self.st.lock().unwrap();
            load_conv(self, &mut st, cid);
            let me = st.me.clone();
            let m = st.msgs.get_mut(cid).and_then(|l| l.iter_mut().find(|m| m.id == mid)).ok_or("No such message")?;
            let who = m.reacts.entry(e.clone()).or_default();
            let on = !who.contains(&me);
            who.retain(|x| *x != me);
            if on {
                who.push(me);
            }
            m.reacts.retain(|_, v| !v.is_empty());
            st.dirty.insert(cid.to_string());
            on
        };
        self.send_small(cid, "react", json!({ "ref": mid, "e": e, "on": on }))?;
        self.bump();
        Ok(json!(on))
    }

    fn delete(&self, cid: &str, mid: &str, everyone: bool) -> Res<Value> {
        let mine = {
            let mut st = self.st.lock().unwrap();
            load_conv(self, &mut st, cid);
            let list = st.msgs.get_mut(cid).ok_or("No such chat")?;
            let pos = list.iter().position(|m| m.id == mid).ok_or("No such message")?;
            let m = list.remove(pos);
            if let Some(p) = &m.pic {
                let _ = fs::remove_file(self.media_path(&p.mid));
            }
            st.dirty.insert(cid.to_string());
            m.mine
        };
        if everyone && mine {
            self.send_small(cid, "del", json!({ "ref": mid }))?;
        }
        self.bump();
        Ok(json!(true))
    }

    /// A view-once picture was looked at: its file goes, the message stays as "Opened".
    fn opened(&self, cid: &str, mid: &str) -> Res<Value> {
        let mut st = self.st.lock().unwrap();
        load_conv(self, &mut st, cid);
        let m = st.msgs.get_mut(cid).and_then(|l| l.iter_mut().find(|m| m.id == mid)).ok_or("No such message")?;
        if let Some(p) = m.pic.as_mut() {
            if p.once && !m.mine {
                p.opened = true;
                let _ = fs::remove_file(self.media_path(&p.mid));
            }
        }
        st.dirty.insert(cid.to_string());
        drop(st);
        self.bump();
        Ok(json!(true))
    }

    #[cfg(test)]
    pub fn media(&self, mid: &str) -> Option<Vec<u8>> {
        crate::crypt::read(&self.media_path(mid)).ok()
    }

    fn add_contact(&self, code: &str, nick: &str) -> Res<Value> {
        let code = code.trim();
        let id = code.rsplit(|c: char| c == ':' || c == '/' || c.is_whitespace()).next().unwrap_or("").to_lowercase();
        let id = id.trim_start_matches(CODE_PREFIX);
        if !is_pubkey(id) {
            return Err("That isn't a Lewd Vault chat code. It starts with lvchat: and 64 letters and numbers.".into());
        }
        {
            let mut st = self.st.lock().unwrap();
            if id == st.me {
                return Err("That's your own code.".into());
            }
            let c = st.saved.contacts.entry(id.to_string()).or_insert_with(|| Contact { added: now(), ..Default::default() });
            c.request = false;
            c.blocked = false;
            if !nick.trim().is_empty() {
                c.nick = clean_nick(nick);
            }
            st.saved.meta.entry(id.to_string()).or_default().hidden = false;
            st.dirty_state = true;
        }
        // a hello, so they see you in their list (with your nickname)
        let _ = self.send_small(id, "hi", json!({ "id": new_id() }));
        self.bump();
        Ok(json!(id))
    }

    fn join_room(&self, name: &str, password: &str) -> Res<Value> {
        let name: String = name.trim().trim_start_matches('#').chars().filter(|c| c.is_alphanumeric() || *c == '-' || *c == '_').take(32).collect::<String>().to_lowercase();
        if name.is_empty() {
            return Err("Give the room a name (letters and numbers).".into());
        }
        let (tag, key) = room_secret(&name, password);
        {
            let mut st = self.st.lock().unwrap();
            st.saved.rooms.entry(tag.clone()).or_insert_with(|| Room { name, key: hex(&key), locked: !password.is_empty(), joined: now() });
            let cid = format!("r:{tag}");
            st.saved.meta.entry(cid).or_default().hidden = false;
            // (a new room: its last week)
            st.saved.since_room = st.saved.since_room.min(now() - ROOM_DAYS * 86400);
            st.dirty_state = true;
        }
        self.resubscribe();
        self.bump();
        Ok(json!(format!("r:{tag}")))
    }

    fn leave(&self, cid: &str) -> Res<Value> {
        {
            let mut st = self.st.lock().unwrap();
            if let Some(tag) = cid.strip_prefix("r:") {
                st.saved.rooms.remove(tag);
            } else if let Some(c) = st.saved.contacts.get(cid) {
                if c.blocked {
                    return Ok(json!(true));
                }
                st.saved.contacts.remove(cid);
            }
            st.saved.meta.remove(cid);
            for m in st.msgs.remove(cid).unwrap_or_default() {
                if let Some(p) = m.pic {
                    let _ = fs::remove_file(self.media_path(&p.mid));
                }
            }
            let _ = fs::remove_file(self.conv_path(cid));
            st.dirty_state = true;
        }
        self.resubscribe();
        self.bump();
        Ok(json!(true))
    }

    fn set(&self, cid: &str, what: &Value) -> Res<Value> {
        {
            let mut st = self.st.lock().unwrap();
            if let Some(c) = st.saved.contacts.get_mut(cid) {
                if let Some(v) = what["fav"].as_bool() { c.fav = v; }
                if let Some(v) = what["blocked"].as_bool() { c.blocked = v; c.request = false; }
                if let Some(v) = what["accept"].as_bool() { if v { c.request = false; } }
                if let Some(v) = what["nick"].as_str() { c.nick = clean_nick(v); }
                if let Some(v) = what["verified"].as_bool() { c.verified = v; }
            }
            let m = st.saved.meta.entry(cid.to_string()).or_default();
            if let Some(v) = what["pinned"].as_bool() { m.pinned = v; }
            if let Some(v) = what["muted"].as_bool() { m.muted = v; }
            if let Some(v) = what["read"].as_bool() { if v { m.unread = 0; } }
            st.dirty_state = true;
        }
        self.bump();
        Ok(json!(true))
    }

    fn resubscribe(&self) {
        self.sub_gen.fetch_add(1, Ordering::Relaxed);
    }
}

/// A disappearing message's end (ms), from its payload: 0 when it stays (at most a month ahead).
fn expiry_of(p: &Value) -> i64 {
    p["exp"].as_i64().filter(|e| *e > 0).map(|e| e.min(now_ms() + 31 * 86400 * 1000)).unwrap_or(0)
}

/// A file extension for a type (the file server serves by extension).
fn ext_for(mime: &str) -> &'static str {
    let m = mime.split(';').next().unwrap_or("").trim().to_ascii_lowercase();
    match m.as_str() {
        "image/jpeg" => "jpg",
        "image/png" => "png",
        "image/gif" => "gif",
        "image/webp" => "webp",
        "video/mp4" | "video/x-m4v" => "mp4",
        "video/quicktime" => "mov",
        "video/webm" | "audio/webm" => "webm",
        "audio/mp4" | "audio/x-m4a" | "audio/aac" => "m4a",
        "audio/mpeg" => "mp3",
        "audio/ogg" | "audio/opus" => "ogg",
        "audio/wav" | "audio/x-wav" => "wav",
        "audio/flac" => "flac",
        _ => "bin",
    }
}

fn accept(st: &mut St, cid: &str) {
    if let Some(c) = st.saved.contacts.get_mut(cid) {
        if c.request {
            c.request = false;
            st.dirty_state = true;
        }
    }
}

fn load_conv(chat: &Chat, st: &mut St, cid: &str) {
    if !st.msgs.contains_key(cid) {
        let t = now_ms();
        let mut list: Vec<Msg> = read_json(&chat.conv_path(cid));
        // (what ended while the chat wasn't loaded: the sweep takes its files next)
        if list.iter().any(|m| m.exp != 0 && m.exp <= t) {
            for m in list.iter().filter(|m| m.exp != 0 && m.exp <= t) {
                for mid in [m.pic.as_ref().map(|p| &p.mid), m.voice.as_ref().map(|v| &v.mid), m.file.as_ref().map(|f| &f.mid)].into_iter().flatten() {
                    if !mid.is_empty() { let _ = fs::remove_file(chat.media_path(mid)); }
                }
            }
            list.retain(|m| m.exp == 0 || m.exp > t);
            st.dirty.insert(cid.to_string());
        }
        st.msgs.insert(cid.to_string(), list);
    }
}

fn add_msg(st: &mut St, cid: &str, mut msg: Msg) -> bool {
    if msg.mine && msg.exp == 0 && msg.sys.is_empty() {
        let ttl = st.saved.meta.get(cid).map(|m| m.ttl).unwrap_or(0);
        if ttl > 0 && !cid.starts_with("r:") {
            msg.exp = msg.at + ttl * 1000;
        }
    }
    if !st.msgs.contains_key(cid) {
        return false; // (load_conv first: see the callers through `known` / chat_api)
    }
    let list = st.msgs.get_mut(cid).unwrap();
    if let Some(old) = list.iter_mut().find(|m| m.id == msg.id) {
        // your own message coming back from a relay (a room): now it's surely out
        if old.mine && old.state == "sending" {
            old.state = "sent".into();
            st.dirty.insert(cid.to_string());
            return true;
        }
        return false;
    }
    let preview = if msg.pic.is_some() {
        if msg.text.is_empty() { "📷 Picture".to_string() } else { format!("📷 {}", msg.text) }
    } else if msg.voice.is_some() {
        "🎤 Voice note".to_string()
    } else if let Some(f) = &msg.file {
        format!("{} {}", if f.kind == "video" { "🎬" } else { "📎" }, f.name)
    } else {
        msg.text.clone()
    };
    let preview = if cid.starts_with("r:") && !msg.mine && !msg.nick.is_empty() { format!("{}: {}", msg.nick, preview) } else { preview };
    let (mine, at, sys) = (msg.mine, msg.at, !msg.sys.is_empty());
    // (in time order: a late arrival goes where it belongs)
    let pos = list.iter().rposition(|m| m.at <= at).map(|p| p + 1).unwrap_or(0);
    list.insert(pos, msg);
    if list.len() > KEEP_MESSAGES {
        let extra = list.len() - KEEP_MESSAGES;
        list.drain(..extra);
    }
    st.dirty.insert(cid.to_string());
    let open = st.open == cid && now_ms() - st.open_at < 5000;
    let m = st.saved.meta.entry(cid.to_string()).or_default();
    if at >= m.last_at {
        m.last_at = at;
        m.last = clip(&preview, 120);
    }
    m.hidden = false;
    if !mine && !open && !sys {
        m.unread += 1;
    }
    st.dirty_state = true;
    true
}

fn set_state(st: &mut St, cid: &str, mid: &str, state: &str) {
    if let Some(m) = st.msgs.get_mut(cid).and_then(|l| l.iter_mut().find(|m| m.id == mid)) {
        if m.state != "sent" || state == "sent" {
            m.state = state.into();
            st.dirty.insert(cid.to_string());
        }
    }
}

// ---------- the page's calls (api_call → "chat…") ----------

pub fn api(dir: &Path, name: &str, args: &[Value], read_item: &dyn Fn(&str) -> Res<Vec<u8>>) -> Res<Value> {
    let a = |k: usize| args.get(k).cloned().unwrap_or(Value::Null);
    let s = |k: usize| a(k).as_str().unwrap_or("").to_string();
    if name == "chatWipe" {
        if let Some(c) = CHAT.lock().unwrap().take() {
            c.shutdown();
        }
        let _ = fs::remove_dir_all(dir);
        return Ok(json!(true));
    }
    // (set up already, on this device or another with the same vault: without starting anything)
    if name == "chatReady" {
        let saved: Saved = read_json(&dir.join("state.json"));
        return Ok(json!(!saved.secret.is_empty() && !saved.nick.is_empty()));
    }
    let chat = get(dir);
    let cid = s(0);
    if !cid.is_empty() && name != "chatAdd" && name != "chatJoin" && name != "chatNick" && name != "chatNearbyTake" {
        let mut st = chat.st.lock().unwrap();
        load_conv(&chat, &mut st, &cid);
    }
    match name {
        "chatState" => {
            let rev = a(0).as_u64().unwrap_or(0);
            if rev != 0 && rev == chat.rev.load(Ordering::Relaxed) {
                return Ok(json!({ "rev": rev }));
            }
            Ok(chat.summary())
        }
        "chatNick" => {
            let mut st = chat.st.lock().unwrap();
            st.saved.nick = clean_nick(&s(0));
            st.dirty_state = true;
            drop(st);
            chat.bump();
            Ok(json!(true))
        }
        "chatQr" => {
            let code = format!("{CODE_PREFIX}{}", chat.st.lock().unwrap().me);
            let qr = qrcode::QrCode::new(code.as_bytes()).map_err(|e| e.to_string())?;
            Ok(json!(qr.render::<qrcode::render::svg::Color>().min_dimensions(220, 220).quiet_zone(true).build()))
        }
        "chatMessages" => {
            let list = chat.messages(&cid, a(2).as_u64().map(|n| n as usize).unwrap_or(KEEP_MESSAGES));
            // (looked at with the app in front: read receipts, if you send them)
            if a(1).as_bool() == Some(true) {
                chat.send_receipts(&cid);
            }
            Ok(list)
        }
        "chatTyping" => {
            if chat.st.lock().unwrap().saved.typing {
                chat.send_now(&cid, "typing", json!({}))?;
            }
            Ok(json!(true))
        }
        "chatPrefs" => {
            let p = a(0);
            let mut st = chat.st.lock().unwrap();
            if let Some(v) = p["receipts"].as_bool() { st.saved.receipts = v; }
            if let Some(v) = p["typing"].as_bool() { st.saved.typing = v; }
            st.dirty_state = true;
            drop(st);
            chat.bump();
            Ok(json!(true))
        }
        "chatSendVoice" => {
            let bytes = base64::engine::general_purpose::STANDARD.decode(s(1).split(',').next_back().unwrap_or("")).map_err(|_| "Bad recording")?;
            chat.send_voice(&cid, &bytes, &s(2), a(3).as_f64().unwrap_or(0.0))
        }
        // the direct link (big files and videos, device to device): the offer, the handshake, the bytes
        "chatFileOffer" => chat.offer_file(&cid, &a(1)),
        "chatSignal" => {
            chat.send_now(&cid, "rtc", json!({ "fid": s(1), "sig": a(2) }))?;
            Ok(json!(true))
        }
        "chatSignals" => Ok(json!(std::mem::take(&mut chat.st.lock().unwrap().signals))),
        "chatFilePart" => {
            let bytes = base64::engine::general_purpose::STANDARD.decode(s(2)).map_err(|_| "Bad piece")?;
            chat.file_part(&cid, &s(1), &bytes, a(3).as_bool().unwrap_or(false))
        }
        "chatFileDone" => chat.file_done(&cid, &s(1)),
        // disappearing messages in a private chat (seconds; 0: off): for both of you
        "chatTtl" => {
            if !is_pubkey(&cid) {
                return Err("Disappearing messages are for private chats.".into());
            }
            let ttl = a(1).as_i64().unwrap_or(0).clamp(0, 30 * 86400);
            let id = new_id();
            {
                let mut st = chat.st.lock().unwrap();
                st.saved.meta.entry(cid.clone()).or_default().ttl = ttl;
                st.dirty_state = true;
                let (me, nick) = (st.me.clone(), st.saved.nick.clone());
                add_msg(&mut st, &cid, Msg { id: id.clone(), from: me, nick, at: now_ms(), sys: format!("ttl:{ttl}"), mine: true, state: "sent".into(), ..Default::default() });
            }
            chat.send_small(&cid, "ttl", json!({ "id": id, "ttl": ttl }))?;
            chat.bump();
            Ok(json!(true))
        }
        // safety numbers (compare them with the person; the same on both phones)
        "chatSafety" => {
            if !is_pubkey(&cid) {
                return Err("Safety numbers are for private chats.".into());
            }
            let st = chat.st.lock().unwrap();
            let (digits, pics) = safety(&st.me, &cid);
            Ok(json!({ "digits": digits, "pics": pics, "verified": st.saved.contacts.get(&cid).map(|c| c.verified).unwrap_or(false) }))
        }
        // (the sender's page, asked for one of its files: what it is and where it comes from)
        "chatFileInfo" => {
            let st = chat.st.lock().unwrap();
            let card = st.msgs.get(&cid).and_then(|l| l.iter().find(|m| m.id == s(1) && m.mine)).and_then(|m| m.file.clone());
            Ok(card.map(|c| json!(c)).unwrap_or(Value::Null))
        }
        "chatClose" => {
            chat.st.lock().unwrap().open.clear();
            Ok(json!(true))
        }
        "chatSend" => chat.send_text(&cid, &s(1), &s(2)),
        "chatSendItem" => {
            let bytes = read_item(&s(1))?;
            chat.send_picture(&cid, &bytes, a(2).as_bool().unwrap_or(false), &s(3))
        }
        "chatSendData" => {
            let bytes = base64::engine::general_purpose::STANDARD.decode(s(1).split(',').next_back().unwrap_or("")).map_err(|_| "Bad picture")?;
            chat.send_picture(&cid, &bytes, a(2).as_bool().unwrap_or(false), &s(3))
        }
        "chatReact" => chat.react(&cid, &s(1), &s(2)),
        "chatDelete" => chat.delete(&cid, &s(1), a(2).as_bool().unwrap_or(false)),
        "chatOpened" => chat.opened(&cid, &s(1)),
        "chatAdd" => chat.add_contact(&s(0), &s(1)),
        "chatJoin" => chat.join_room(&s(0), &s(1)),
        "chatLeave" => chat.leave(&cid),
        "chatSet" => chat.set(&cid, &a(1)),
        "chatRelays" => {
            // (your own list: exactly those relays; an empty one: automatic again)
            let list: Vec<String> = a(0).as_array().cloned().unwrap_or_default().iter().filter_map(|v| v.as_str())
                .map(|u| u.trim().trim_end_matches('/').to_string()).filter(|u| pool::allowed(u)).take(12).collect();
            if list.is_empty() && a(0).as_array().map(|x| !x.is_empty()).unwrap_or(false) {
                return Err("Relays need an encrypted address: wss://…".into());
            }
            {
                let mut st = chat.st.lock().unwrap();
                st.saved.manual = !list.is_empty();
                st.saved.relays = list;
                st.dirty_state = true;
            }
            for l in chat.links.lock().unwrap().iter() {
                l.stop.store(true, Ordering::Relaxed);
            }
            chat.bump();
            Ok(json!(true))
        }
        "chatRetry" => {
            // (the outbox again, now: for "Not sent, tap to retry")
            let mut st = chat.st.lock().unwrap();
            for o in st.saved.outbox.iter_mut() {
                o.at = now();
            }
            if let Some(m) = st.msgs.get_mut(&cid).and_then(|l| l.iter_mut().find(|m| m.id == s(1))) {
                if m.state == "failed" { m.state = "sending".into(); }
            }
            drop(st);
            for f in chat.outbox_events() {
                chat.broadcast(&f);
            }
            Ok(json!(true))
        }
        // Nearby (Bluetooth): the native side moves sealed events between phones; these wrap and take them
        "chatNearbyHello" => {
            let st = chat.st.lock().unwrap();
            Ok(json!({ "id": st.me, "nick": st.saved.nick }))
        }
        "chatNearbyWrap" => {
            // (args: the person's id, then what to send: a text, or a vault item's id with "item")
            let to = s(0);
            if !is_pubkey(&to) {
                return Err("Bad key".into());
            }
            {
                let mut st = chat.st.lock().unwrap();
                st.saved.contacts.entry(to.clone()).or_insert_with(|| Contact { added: now(), nick: clean_nick(&s(3)), ..Default::default() });
                load_conv(&chat, &mut st, &to);
            }
            let id = if s(2) == "item" { chat.send_picture_sized(&to, &read_item(&s(1))?, false, "", true)? } else { chat.send_text(&to, &s(1), "")? };
            // the same sealed events that went to the relays, for the radio too
            let st = chat.st.lock().unwrap();
            let evs: Vec<String> = st.saved.outbox.iter().filter(|o| o.cid == to && o.mid == id.as_str().unwrap_or("")).map(|o| o.ev.json()).collect();
            Ok(json!(evs))
        }
        "chatNearbyTake" => {
            let ev: Event = serde_json::from_str(&s(0)).map_err(|_| "Bad event")?;
            Ok(json!(chat.take(ev, "nearby")))
        }
        _ => Err(format!("Unknown call: {name}")),
    }
}

/// /chat/<id>: a chat's picture, voice note or file, for the file server (which reads it in ranges).
pub fn media_file(mid: &str) -> Option<PathBuf> {
    CHAT.lock().unwrap().as_ref().map(|c| c.media_path(mid)).filter(|p| p.exists())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sign_and_verify() {
        let k = new_key();
        let ev = Event::new(pub_of(&k), now(), 1, vec![vec!["x".into(), "y".into()]], "hello \"there\"\n/é".into()).sign(&k);
        assert!(ev.verify());
        let mut bad = ev.clone();
        bad.content.push('!');
        assert!(!bad.verify());
    }

    #[test]
    fn known_signature_vector() {
        // BIP-340 test vector 0 (what relays check every event against)
        let sk = SigningKey::from_bytes(&unhex("0000000000000000000000000000000000000000000000000000000000000003").unwrap()).unwrap();
        assert_eq!(pub_of(&sk), "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9");
        let vk = VerifyingKey::from_bytes(&unhex("f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9").unwrap()).unwrap();
        let sig = Signature::try_from(unhex("e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca821525f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0").unwrap().as_slice()).unwrap();
        assert!(vk.verify_raw(&[0u8; 32], &sig).is_ok());
    }

    #[test]
    fn shared_key_both_ways() {
        for _ in 0..20 {
            let (a, b) = (new_key(), new_key());
            assert_eq!(shared_key(&a, &pub_of(&b)).unwrap(), shared_key(&b, &pub_of(&a)).unwrap());
        }
    }

    #[test]
    fn wrap_round_trip_and_checks() {
        let (alice, bob, eve) = (new_key(), new_key(), new_key());
        let ev = wrap(&alice, &pub_of(&bob), "{\"t\":\"msg\",\"text\":\"hi\"}").unwrap();
        assert_eq!(ev.kind, KIND_WRAP);
        assert!(ev.verify());
        assert_ne!(ev.pubkey, pub_of(&alice)); // (the relay can't see who sent it)
        assert!(!ev.content.contains("\"hi\"")); // (a quote can't be in base64: the words aren't there)
        let (from, _, payload) = unwrap(&bob, &ev).unwrap();
        assert_eq!(from, pub_of(&alice));
        assert!(payload.contains("\"hi\""));
        assert!(unwrap(&eve, &ev).is_err());
        let mut tampered = ev.clone();
        tampered.content = tampered.content.replace('A', "B");
        assert!(unwrap(&bob, &tampered).is_err());
    }

    #[test]
    fn rooms_need_the_name_and_password() {
        let (t1, k1) = room_secret("#Lounge", "");
        let (t2, k2) = room_secret("lounge", "");
        let (t3, _) = room_secret("lounge", "pw");
        assert_eq!((t1.clone(), k1), (t2, k2));
        assert_ne!(t1, t3);
        let me = new_key();
        let ev = room_event(&me, &t1, &k1, "{\"t\":\"msg\"}");
        assert!(ev.verify());
        assert_eq!(open_box(&k1, &ev.content).unwrap(), b"{\"t\":\"msg\"}");
    }

    #[test]
    fn pictures_shrink() {
        let img = image::RgbImage::from_fn(3000, 2000, |x, y| image::Rgb([(x % 256) as u8, (y % 256) as u8, ((x * y) % 256) as u8]));
        let mut png = Vec::new();
        image::DynamicImage::ImageRgb8(img).write_to(&mut std::io::Cursor::new(&mut png), image::ImageFormat::Png).unwrap();
        let (w, h, jpeg) = prepare_picture(&png, false).unwrap();
        assert!(w <= 1280 && h <= 1280 && jpeg.len() <= MAX_PICTURE);
        let (w, h, _) = prepare_picture(&png, true).unwrap();
        assert!(w <= 800 && h <= 800);
    }

    /// Two people, a little relay of our own: a message, a picture in pieces, a reaction, a room.
    #[test]
    fn two_people_through_a_relay() {
        let port = test_relay();
        let base = std::env::temp_dir().join(format!("lv-chat-test-{}", new_id()));
        let (da, db) = (base.join("a"), base.join("b"));
        let a = Chat::open(da.clone());
        let b = Chat::open(db.clone());
        for c in [&a, &b] {
            let mut st = c.st.lock().unwrap();
            st.saved.relays = vec![format!("ws://127.0.0.1:{port}")];
            st.saved.manual = true;
            drop(st);
            c.connect();
        }
        let none = |_: &str| -> Res<Vec<u8>> { Err("none".into()) };
        let call = |c: &Arc<Chat>, d: &Path, n: &str, args: Value| -> Value {
            // (the module's own entry point would start a second chat for the folder: call the methods)
            let _ = d;
            let args = args.as_array().cloned().unwrap_or_default();
            let cid = args.first().and_then(|v| v.as_str()).unwrap_or("").to_string();
            if !cid.is_empty() { let mut st = c.st.lock().unwrap(); load_conv(c, &mut st, &cid); }
            match n {
                "add" => c.add_contact(&cid, "").unwrap(),
                "send" => c.send_text(&cid, args[1].as_str().unwrap(), "").unwrap(),
                "msgs" => c.messages(&cid, 500),
                "join" => c.join_room(&cid, "").unwrap(),
                _ => { let _ = none; Value::Null }
            }
        };
        let (pa, pb) = (a.st.lock().unwrap().me.clone(), b.st.lock().unwrap().me.clone());
        a.st.lock().unwrap().saved.nick = "Alice".into();
        call(&a, &da, "add", json!([format!("lvchat:{pb}")]));
        call(&a, &da, "send", json!([pb, "hello Bob"]));
        let wait = |f: &dyn Fn() -> bool| { for _ in 0..100 { if f() { return true; } std::thread::sleep(Duration::from_millis(100)); } false };
        assert!(wait(&|| { let mut st = b.st.lock().unwrap(); load_conv(&b, &mut st, &pa); st.msgs[&pa].iter().any(|m| m.text == "hello Bob") }), "Bob got the message");
        assert_eq!(b.st.lock().unwrap().saved.contacts[&pa].nick, "Alice");
        assert!(wait(&|| a.st.lock().unwrap().msgs[&pb].iter().any(|m| m.state == "sent")), "Alice's shows sent");
        // a picture, in pieces
        let img = image::RgbImage::from_fn(900, 700, |x, y| image::Rgb([(x * 7 % 256) as u8, (y * 3 % 256) as u8, ((x ^ y) % 256) as u8]));
        let mut png = Vec::new();
        image::DynamicImage::ImageRgb8(img).write_to(&mut std::io::Cursor::new(&mut png), image::ImageFormat::Png).unwrap();
        b.send_picture(&pa, &png, true, "for you").unwrap();
        assert!(wait(&|| a.st.lock().unwrap().msgs[&pb].iter().any(|m| m.pic.is_some() && m.loading.is_none())), "Alice got the picture");
        let pic = a.st.lock().unwrap().msgs[&pb].iter().find_map(|m| m.pic.clone()).unwrap();
        assert!(pic.once && a.media(&pic.mid).map(|b| b.len() > 1000).unwrap_or(false));
        // a reaction
        let mid = b.st.lock().unwrap().msgs[&pa].iter().find(|m| m.text == "hello Bob").unwrap().id.clone();
        b.react(&pa, &mid, "♥").unwrap();
        assert!(wait(&|| a.st.lock().unwrap().msgs[&pb].iter().any(|m| m.reacts.get("♥").map(|v| v.contains(&pb)).unwrap_or(false))), "the heart arrived");
        // a room both join
        let room = call(&a, &da, "join", json!(["lounge"])).as_str().unwrap().to_string();
        call(&b, &db, "join", json!(["Lounge"]));
        std::thread::sleep(Duration::from_millis(400));
        { let mut st = b.st.lock().unwrap(); load_conv(&b, &mut st, &room); }
        a.send_text(&room, "hi room", "").unwrap();
        assert!(wait(&|| b.st.lock().unwrap().msgs[&room].iter().any(|m| m.text == "hi room" && m.nick == "Alice")), "the room message arrived");
        // read receipts (Bob allows them): Alice's message shows "read"
        b.st.lock().unwrap().saved.receipts = true;
        let mid2 = a.send_text(&pb, "did you see this?", "").unwrap().as_str().unwrap().to_string();
        assert!(wait(&|| b.st.lock().unwrap().msgs[&pa].iter().any(|m| m.id == mid2)), "Bob got it");
        b.messages(&pa, 50);
        b.send_receipts(&pa);
        assert!(wait(&|| a.st.lock().unwrap().msgs[&pb].iter().any(|m| m.id == mid2 && m.state == "read")), "Alice sees it read");
        // typing (Bob allows it): ephemeral, nothing kept by the relay's store matters
        b.st.lock().unwrap().saved.typing = true;
        b.send_now(&pa, "typing", json!({})).unwrap();
        assert!(wait(&|| a.st.lock().unwrap().typing.contains_key(&pb)), "Alice sees Bob typing");
        // a voice note
        let rec: Vec<u8> = (0..20000u32).map(|i| (i * 7 % 251) as u8).collect();
        a.send_voice(&pb, &rec, "audio/webm;codecs=opus", 3.2).unwrap();
        assert!(wait(&|| b.st.lock().unwrap().msgs[&pa].iter().any(|m| m.voice.as_ref().map(|v| v.secs == 3.2 && v.mid.ends_with(".webm")).unwrap_or(false))), "Bob got the voice note");
        let v = b.st.lock().unwrap().msgs[&pa].iter().find_map(|m| m.voice.clone()).unwrap();
        assert_eq!(b.media(&v.mid).unwrap(), rec);
        // a big file: the card goes through the relay (without the sender's vault item), the bytes don't
        let fid = a.offer_file(&pb, &json!({ "name": "clip.mp4", "size": 3000, "mime": "video/mp4", "kind": "video", "src": "item-000001" })).unwrap().as_str().unwrap().to_string();
        assert!(wait(&|| b.st.lock().unwrap().msgs[&pa].iter().any(|m| m.file.as_ref().map(|f| f.fid == fid).unwrap_or(false))), "Bob got the offer");
        let card = b.st.lock().unwrap().msgs[&pa].iter().find_map(|m| m.file.clone()).unwrap();
        assert!(card.src.is_empty() && card.size == 3000 && card.kind == "video");
        // the handshake for the direct link
        b.send_now(&pa, "rtc", json!({ "fid": fid, "sig": { "kind": "offer", "sdp": "v=0" } })).unwrap();
        assert!(wait(&|| a.st.lock().unwrap().signals.iter().any(|s| s["fid"] == fid.as_str() && s["sig"]["kind"] == "offer")), "Alice got the handshake");
        // the bytes, as the page writes them after the direct link
        let bytes: Vec<u8> = (0..3000u32).map(|i| (i % 256) as u8).collect();
        b.file_part(&pa, &fid, &bytes[..1000], true).unwrap();
        assert!(b.file_done(&pa, &fid).is_err(), "not whole yet");
        b.file_part(&pa, &fid, &bytes[1000..], false).unwrap();
        b.file_done(&pa, &fid).unwrap();
        let card = b.st.lock().unwrap().msgs[&pa].iter().find_map(|m| m.file.clone()).unwrap();
        assert!(card.done && card.mid.ends_with(".mp4"));
        assert_eq!(b.media(&card.mid).unwrap(), bytes);
        // disappearing messages: set by Bob, both sides follow; the relay is told when each ends
        let mut st = b.st.lock().unwrap(); st.saved.meta.entry(pa.clone()).or_default().ttl = 2; drop(st);
        b.send_small(&pa, "ttl", json!({ "id": new_id(), "ttl": 2 })).unwrap();
        assert!(wait(&|| a.st.lock().unwrap().saved.meta.get(&pb).map(|m| m.ttl == 2).unwrap_or(false)), "Alice follows the timer");
        let gone = a.send_text(&pb, "this will vanish", "").unwrap().as_str().unwrap().to_string();
        let ev = a.st.lock().unwrap().saved.outbox.iter().find(|o| o.mid == gone).map(|o| o.ev.clone()).unwrap();
        assert!(ev.tag("expiration").is_some(), "the relay is told when it ends");
        assert!(wait(&|| b.st.lock().unwrap().msgs[&pa].iter().any(|m| m.id == gone && m.exp > 0)), "Bob got it, with its end");
        std::thread::sleep(Duration::from_millis(2100));
        a.sweep();
        b.sweep();
        assert!(!a.st.lock().unwrap().msgs[&pb].iter().any(|m| m.id == gone) && !b.st.lock().unwrap().msgs[&pa].iter().any(|m| m.id == gone), "gone on both sides");
        // the files are written, and come back
        a.flush();
        let again = Chat::open(da.clone());
        assert_eq!(again.st.lock().unwrap().me, pa);
        a.shutdown();
        b.shutdown();
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn padding_hides_length() {
        let sizes: Vec<usize> = ["hi", "hello there, how are you", &"x".repeat(200), &"y".repeat(300)].iter()
            .map(|t| padded(&json!({ "t": "msg", "text": t })).len()).collect();
        assert_eq!(sizes, vec![256, 256, 256, 512]);
        let p: Value = serde_json::from_str(&padded(&json!({ "t": "msg", "text": "hi" }))).unwrap();
        assert_eq!(p["text"], "hi");
    }

    #[test]
    fn safety_numbers_match() {
        let (a, b, c) = (pub_of(&new_key()), pub_of(&new_key()), pub_of(&new_key()));
        assert_eq!(safety(&a, &b), safety(&b, &a));
        assert_ne!(safety(&a, &b).0, safety(&a, &c).0);
        let (digits, pics) = safety(&a, &b);
        assert!(digits.len() == 12 && digits.iter().all(|d| d.len() == 5 && d.chars().all(|c| c.is_ascii_digit())));
        assert_eq!(pics.len(), 8);
    }

    /// A picture's pieces in any order: nothing until the first piece (which says what it is), then a
    /// blurred preview with progress, then the picture.
    #[test]
    fn pictures_show_while_they_arrive() {
        let base = std::env::temp_dir().join(format!("lv-prog-{}", new_id()));
        let (a, b) = (Chat::open(base.join("a")), Chat::open(base.join("b")));
        let (pa, pb) = (a.st.lock().unwrap().me.clone(), b.st.lock().unwrap().me.clone());
        { let mut st = b.st.lock().unwrap(); load_conv(&b, &mut st, &pa); }
        let img = image::RgbImage::from_fn(1200, 900, |x, y| image::Rgb([(x * 13 % 256) as u8, (y * 7 % 256) as u8, ((x * y) % 256) as u8]));
        let mut png = Vec::new();
        image::DynamicImage::ImageRgb8(img).write_to(&mut std::io::Cursor::new(&mut png), image::ImageFormat::Png).unwrap();
        b.send_picture(&pa, &png, false, "").unwrap();
        let mut evs: Vec<Event> = b.st.lock().unwrap().saved.outbox.iter().map(|o| o.ev.clone()).collect();
        assert!(evs.len() >= 3, "several pieces ({})", evs.len());
        { let mut st = a.st.lock().unwrap(); load_conv(&a, &mut st, &pb); }
        let last = evs.pop().unwrap();
        a.take(last, "net");
        assert!(a.st.lock().unwrap().msgs[&pb].is_empty(), "nothing until the first piece");
        let first = evs.remove(0);
        a.take(first, "net");
        let m = a.st.lock().unwrap().msgs[&pb][0].clone();
        let l = m.loading.expect("a preview while it arrives");
        assert!(l.got == 2 && l.thumb.starts_with("data:image/jpeg;base64,") && m.pic.as_ref().unwrap().mid.is_empty());
        for e in evs {
            a.take(e, "net");
        }
        let m = a.st.lock().unwrap().msgs[&pb][0].clone();
        assert!(m.loading.is_none() && !m.pic.unwrap().mid.is_empty(), "now the picture");
        assert_eq!(a.st.lock().unwrap().msgs[&pb].len(), 1);
        let _ = fs::remove_dir_all(&base);
    }

    /// The page's calls, through the real entry point (each under a timeout: a lock taken twice would hang).
    #[test]
    fn page_calls_never_hang() {
        let dir = std::env::temp_dir().join(format!("lv-chat-api-{}", new_id()));
        fs::create_dir_all(&dir).unwrap();
        // (a relay nobody listens on: no network in tests)
        fs::write(dir.join("state.json"), json!({ "relays": ["ws://127.0.0.1:9"], "manual": true }).to_string()).unwrap();
        let other = new_key();
        let none = |_: &str| -> Res<Vec<u8>> { Err("no items".into()) };
        let call = |name: &str, args: Value| -> Value {
            let (tx, rx) = mpsc::channel();
            let (d, n, a) = (dir.clone(), name.to_string(), args.as_array().cloned().unwrap_or_default());
            std::thread::spawn(move || { let _ = tx.send(api(&d, &n, &a, &none)); });
            rx.recv_timeout(Duration::from_secs(10)).unwrap_or_else(|_| panic!("{name} hung")).unwrap_or_else(|e| json!({ "error": e }))
        };
        assert_eq!(call("chatReady", json!([])), json!(false));
        call("chatNick", json!(["Tester"]));
        let st = call("chatState", json!([0]));
        assert_eq!(st["me"]["nick"], "Tester");
        assert_eq!(st["pool"]["mode"], "manual");
        assert_eq!(st["pool"]["yours"][0], "ws://127.0.0.1:9");
        // (unchanged: just the number; a relay thread may have bumped it in between, then it's the whole)
        let again = call("chatState", json!([st["rev"]]));
        assert!(again["rev"].is_u64() && (again["convs"].is_null() || again["rev"] != st["rev"]));
        let me = st["me"]["id"].as_str().unwrap().to_string();
        let cid = call("chatAdd", json!([format!("lvchat:{}", pub_of(&other)), "Other"])).as_str().unwrap().to_string();
        call("chatSend", json!([cid, "hello"]));
        assert_eq!(call("chatMessages", json!([cid]))[0]["text"], "hello");
        // something arriving from a nearby phone
        let ev = wrap(&other, &me, &json!({ "v": 1, "t": "msg", "id": "n1", "text": "from nearby", "nick": "Other" }).to_string()).unwrap();
        assert_eq!(call("chatNearbyTake", json!([ev.json()])), json!(true));
        assert_eq!(call("chatNearbyTake", json!([ev.json()])), json!(false)); // (once)
        let msgs = call("chatMessages", json!([cid]));
        assert!(msgs.as_array().unwrap().iter().any(|m| m["text"] == "from nearby" && m["via"] == "nearby"));
        let evs = call("chatNearbyWrap", json!([cid, "back at you", "text", "Other"]));
        let evs: Vec<Event> = evs.as_array().unwrap().iter().map(|e| serde_json::from_str(e.as_str().unwrap()).unwrap()).collect();
        assert!(evs.iter().any(|e| unwrap(&other, e).map(|(_, _, p)| p.contains("back at you")).unwrap_or(false)));
        call("chatJoin", json!(["lounge", ""]));
        call("chatQr", json!([]));
        assert!(call("chatState", json!([0]))["convs"].as_array().unwrap().len() == 2);
        call("chatWipe", json!([]));
        assert!(!dir.exists());
    }

    /// A minimal Nostr relay (NIP-01: EVENT, REQ with kinds/#p/#r/since, OK, EOSE), for the tests.
    pub fn test_relay() -> u16 {
        test_relay_on(0)
    }

    /// A test relay that can be switched off (its port, and the switch).
    pub fn test_relay_ctl() -> (u16, Arc<AtomicBool>) {
        let t = TestRelay::start();
        (t.port, t.off)
    }

    /// A test relay with its controls: off, tamper (it alters the events it hands over), and how
    /// many events were published to it.
    pub struct TestRelay {
        pub port: u16,
        pub off: Arc<AtomicBool>,
        pub tamper: Arc<AtomicBool>,
        pub published: Arc<std::sync::atomic::AtomicUsize>,
    }
    impl TestRelay {
        pub fn start() -> TestRelay {
            let t = TestRelay { port: 0, off: Default::default(), tamper: Default::default(), published: Default::default() };
            let port = relay_with(0, t.off.clone(), t.tamper.clone(), t.published.clone());
            TestRelay { port, ..t }
        }
        pub fn url(&self) -> String {
            format!("ws://127.0.0.1:{}", self.port)
        }
    }

    /// The test relay on its own, for trying the chat screens by hand or in the screenshot harness:
    /// RELAY_PORT=7447 cargo test chat_relay_server -- --ignored, then LV_CHAT_RELAYS=ws://127.0.0.1:7447
    #[test]
    #[ignore]
    fn chat_relay_server() {
        let port = test_relay_on(std::env::var("RELAY_PORT").ok().and_then(|p| p.parse().ok()).unwrap_or(7447));
        println!("relay on {port}");
        loop {
            std::thread::sleep(Duration::from_secs(3600));
        }
    }

    pub fn test_relay_on(port: u16) -> u16 {
        relay_with(port, Default::default(), Default::default(), Default::default())
    }

    fn relay_with(port: u16, off: Arc<AtomicBool>, tamper: Arc<AtomicBool>, published: Arc<std::sync::atomic::AtomicUsize>) -> u16 {
        use std::net::TcpListener;
        let listener = TcpListener::bind(("127.0.0.1", port)).unwrap();
        let port = listener.local_addr().unwrap().port();
        listener.set_nonblocking(true).unwrap();
        let store: Arc<Mutex<Vec<Event>>> = Arc::new(Mutex::new(Vec::new()));
        let clients: Arc<Mutex<Vec<mpsc::Sender<Event>>>> = Arc::new(Mutex::new(Vec::new()));
        std::thread::spawn(move || {
            loop {
                if off.load(Ordering::Relaxed) {
                    return; // (switched off: no more connections, and the open ones close below)
                }
                let stream = match listener.accept() {
                    Ok((s, _)) => s,
                    Err(_) => { std::thread::sleep(Duration::from_millis(20)); continue; }
                };
                stream.set_nonblocking(false).ok();
                let (store, clients, off, tamper, published) = (store.clone(), clients.clone(), off.clone(), tamper.clone(), published.clone());
                std::thread::spawn(move || {
                    let Ok(mut ws) = tungstenite::accept(stream.try_clone().unwrap()) else { return };
                    stream.set_read_timeout(Some(Duration::from_millis(50))).ok();
                    let (tx, rx) = mpsc::channel::<Event>();
                    clients.lock().unwrap().push(tx);
                    // (subscriptions by id, as relays keep them: a new REQ with another id adds one)
                    let mut subs: HashMap<String, Vec<Value>> = HashMap::new();
                    let matches = |f: &Value, e: &Event| {
                        f["kinds"].as_array().map(|k| k.iter().any(|x| x.as_u64() == Some(e.kind as u64))).unwrap_or(true)
                            && f["authors"].as_array().map(|a| a.iter().any(|x| x.as_str() == Some(e.pubkey.as_str()))).unwrap_or(true)
                            && f["#p"].as_array().map(|p| p.iter().any(|x| Some(x.as_str().unwrap_or("")) == e.tag("p"))).unwrap_or(true)
                            && f["#r"].as_array().map(|p| p.iter().any(|x| Some(x.as_str().unwrap_or("")) == e.tag("r"))).unwrap_or(true)
                            && f["since"].as_i64().map(|s| e.created_at >= s).unwrap_or(true)
                    };
                    loop {
                        if off.load(Ordering::Relaxed) {
                            return;
                        }
                        while let Ok(mut e) = rx.try_recv() {
                            if tamper.load(Ordering::Relaxed) {
                                e.content.push('x'); // (a relay that changes what it passes on)
                            }
                            for (id, fs) in &subs {
                                if fs.iter().any(|f| matches(f, &e)) && ws.send(tungstenite::Message::text(json!(["EVENT", id, e]).to_string())).is_err() { return; }
                            }
                        }
                        match ws.read() {
                            Ok(tungstenite::Message::Text(t)) => {
                                let v: Value = serde_json::from_str(t.as_str()).unwrap_or(Value::Null);
                                match v[0].as_str() {
                                    Some("EVENT") => {
                                        published.fetch_add(1, Ordering::Relaxed);
                                        let e: Event = serde_json::from_value(v[1].clone()).unwrap();
                                        let ok = e.verify();
                                        let _ = ws.send(tungstenite::Message::text(json!(["OK", e.id, ok, ""]).to_string()));
                                        if ok {
                                            store.lock().unwrap().push(e.clone());
                                            for c in clients.lock().unwrap().iter() { let _ = c.send(e.clone()); }
                                        }
                                    }
                                    Some("REQ") => {
                                        let id = v[1].as_str().unwrap_or("").to_string();
                                        let fs = v.as_array().unwrap()[2..].to_vec();
                                        for e in store.lock().unwrap().iter() {
                                            // (ephemeral kinds are never stored: never replayed)
                                            if e.kind < 20000 && fs.iter().any(|f| matches(f, e)) {
                                                let _ = ws.send(tungstenite::Message::text(json!(["EVENT", id, e]).to_string()));
                                            }
                                        }
                                        let _ = ws.send(tungstenite::Message::text(json!(["EOSE", id]).to_string()));
                                        subs.insert(id, fs);
                                    }
                                    Some("CLOSE") => { subs.remove(v[1].as_str().unwrap_or("")); }
                                    _ => {}
                                }
                            }
                            Ok(_) => {}
                            Err(tungstenite::Error::Io(e)) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => { let _ = ws.flush(); }
                            Err(_) => return,
                        }
                    }
                });
            }
        });
        port
    }
}
