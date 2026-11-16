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

type Res<T> = Result<T, String>;

const KIND_SEAL: u32 = 13;
const KIND_RUMOR: u32 = 14;
const KIND_WRAP: u32 = 1059;
const KIND_ROOM: u32 = 4573;
const KIND_AUTH: u32 = 22242;
/// (the same public relays bitchat starts with; editable in the chat's settings)
const DEFAULT_RELAYS: &[&str] = &["wss://relay.damus.io", "wss://nos.lol", "wss://relay.primal.net", "wss://offchain.pub"];
const MAX_ENVELOPE: usize = 96 * 1024;
const PIECE_DM: usize = 14 * 1024;
const PIECE_ROOM: usize = 24 * 1024;
const MAX_PICTURE: usize = 420 * 1024;
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
pub fn wrap(me: &SigningKey, to: &str, payload: &str) -> Res<Event> {
    let my_pub = pub_of(me);
    let mut rumor = Event::new(my_pub.clone(), now(), KIND_RUMOR, vec![], payload.to_string());
    rumor.id = hex(&rumor.hash());
    let seal = Event::new(my_pub, jittered(), KIND_SEAL, vec![], lock_box(&shared_key(me, to)?, rumor.json().as_bytes())).sign(me);
    let once = new_key();
    let wrapped = lock_box(&shared_key(&once, to)?, seal.json().as_bytes());
    Ok(Event::new(pub_of(&once), jittered(), KIND_WRAP, vec![vec!["p".into(), to.into()]], wrapped).sign(&once))
}

/// The other way (bitchat's decryptPrivateMessage, with its checks): (sender, time sent, payload).
pub fn unwrap(me: &SigningKey, ev: &Event) -> Res<(String, i64, String)> {
    let my_pub = pub_of(me);
    if ev.kind != KIND_WRAP || ev.content.len() > MAX_ENVELOPE * 2 || ev.tags != vec![vec!["p".to_string(), my_pub.clone()]] || !ev.verify() {
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

/// Pictures: no bigger than 1280 px (then 1024, 800) and MAX_PICTURE bytes, as JPEG. (w, h, bytes)
/// (`small`: for Nearby, where Bluetooth is slow, 800 px at most)
pub fn prepare_picture(bytes: &[u8], small: bool) -> Res<(u32, u32, Vec<u8>)> {
    let img = image::load_from_memory(bytes).map_err(|_| "Only pictures can be sent for now.")?;
    let ladder: &[(u32, u8)] = if small { &[(800, 68), (640, 58), (480, 50)] } else { &[(1280, 80), (1024, 72), (800, 65), (640, 55)] };
    for &(side, quality) in ladder {
        let small = if img.width().max(img.height()) > side { img.thumbnail(side, side) } else { img.clone() };
        let rgb = small.to_rgb8();
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
    receipts: bool,
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
struct Msg {
    id: String,
    from: String,
    nick: String,
    at: i64,
    text: String,
    pic: Option<Picture>,
    reply: String,
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
    open: String,
}

struct RelayLink {
    tx: mpsc::Sender<String>,
    stop: Arc<AtomicBool>,
}

pub struct Chat {
    dir: PathBuf,
    st: Mutex<St>,
    links: Mutex<Vec<RelayLink>>,
    up: Mutex<HashMap<String, bool>>,
    rev: AtomicU64,
    sub_gen: AtomicU64,
    stopped: AtomicBool,
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
        if saved.relays.is_empty() {
            saved.relays = DEFAULT_RELAYS.iter().map(|s| s.to_string()).collect();
        }
        if saved.since_dm == 0 {
            saved.since_dm = now() - 86400;
            saved.since_room = now() - ROOM_DAYS * 86400;
        }
        let me = pub_of(&key);
        Arc::new(Chat {
            dir,
            st: Mutex::new(St { saved, key, me, msgs: HashMap::new(), dirty: HashSet::new(), dirty_state: true, parts: HashMap::new(), rate: HashMap::new(), open: String::new() }),
            links: Mutex::new(Vec::new()),
            up: Mutex::new(HashMap::new()),
            rev: AtomicU64::new(1),
            sub_gen: AtomicU64::new(1),
            stopped: AtomicBool::new(false),
        })
    }

    fn bump(&self) {
        self.rev.fetch_add(1, Ordering::Relaxed);
    }

    fn relay_list(&self) -> Vec<String> {
        let saved = self.st.lock().unwrap().saved.relays.clone();
        relays_or_override(saved)
    }

    fn connect(self: &Arc<Self>) {
        let mut links = self.links.lock().unwrap();
        for l in links.drain(..) {
            l.stop.store(true, Ordering::Relaxed);
        }
        for url in self.relay_list() {
            let (tx, rx) = mpsc::channel();
            let stop = Arc::new(AtomicBool::new(false));
            let (me, u, s) = (self.clone(), url, stop.clone());
            std::thread::spawn(move || relay_loop(me, u, rx, s));
            links.push(RelayLink { tx, stop });
        }
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
        std::thread::spawn(move || loop {
            std::thread::sleep(Duration::from_millis(600));
            if me.stopped.load(Ordering::Relaxed) {
                return;
            }
            me.flush();
        });
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
    fn media_path(&self, mid: &str) -> PathBuf {
        self.dir.join(format!("m-{}.bin", mid.chars().filter(|c| c.is_ascii_alphanumeric()).collect::<String>()))
    }

    // ---------- the subscriptions every relay is asked for ----------

    fn filters(&self) -> Vec<Value> {
        let st = self.st.lock().unwrap();
        // (wraps are back-dated up to 15 minutes, and a phone may be off for a while: two days of margin)
        let mut f = vec![json!({ "kinds": [KIND_WRAP], "#p": [st.me], "since": st.saved.since_dm - 2 * 86400 })];
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
        for l in self.links.lock().unwrap().iter() {
            for f in &frames {
                let _ = l.tx.send(f.clone());
            }
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
        if ev.content.len() > MAX_ENVELOPE * 2 || !matches!(ev.kind, KIND_WRAP | KIND_ROOM) {
            return false;
        }
        let mut st = self.st.lock().unwrap();
        if st.saved.seen.contains(&ev.id) {
            return false;
        }
        let got = match ev.kind {
            KIND_WRAP => unwrap(&st.key, &ev).ok().map(|(from, at, payload)| {
                st.saved.since_dm = st.saved.since_dm.max(ev.created_at.min(now()));
                (None, from, at, payload)
            }),
            _ => {
                let tag = ev.tag("r").unwrap_or_default().to_string();
                let key = st.saved.rooms.get(&tag).and_then(|r| unhex(&r.key)).and_then(|k| <[u8; 32]>::try_from(k).ok());
                match key {
                    Some(k) if ev.verify() => open_box(&k, &ev.content).ok().and_then(|b| String::from_utf8(b).ok()).map(|p| {
                        st.saved.since_room = st.saved.since_room.max(ev.created_at.min(now()));
                        (Some(tag), ev.pubkey.clone(), ev.created_at, p)
                    }),
                    _ => None,
                }
            }
        };
        st.saved.seen.push_back(ev.id.clone());
        while st.saved.seen.len() > 6000 {
            st.saved.seen.pop_front();
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
        match p["t"].as_str().unwrap_or("") {
            "msg" | "hi" => {
                if id.is_empty() {
                    return false;
                }
                let text = clip(p["text"].as_str().unwrap_or(""), 4000);
                if p["t"] == "hi" && text.is_empty() {
                    return room.is_none() && !mine; // (a hello: the contact appears, nothing to show)
                }
                let reply = clip(p["reply"].as_str().unwrap_or(""), 32);
                add_msg(st, &cid, Msg { id, from: from.into(), nick, at: at_ms, text, reply, mine, state: if mine { "sent".into() } else { "received".into() }, via: via.into(), ..Default::default() })
            }
            "img" => {
                let (n, i) = (p["n"].as_u64().unwrap_or(0) as usize, p["i"].as_u64().unwrap_or(0) as usize);
                if id.is_empty() || n == 0 || n > 64 || i >= n {
                    return false;
                }
                if st.msgs.get(&cid).map(|m| m.iter().any(|x| x.id == id)).unwrap_or(false) || self.known(st, &cid, &id) {
                    return false;
                }
                let Ok(piece) = base64::engine::general_purpose::STANDARD.decode(p["d"].as_str().unwrap_or("")) else { return false };
                let key = format!("{cid}/{from}/{id}");
                let part = st.parts.entry(key.clone()).or_insert_with(|| Partial { pieces: vec![None; n], first: p.clone(), at: at_ms, started: now() });
                if part.pieces.len() != n {
                    return false;
                }
                part.pieces[i] = Some(piece);
                if part.pieces.iter().any(|x| x.is_none()) {
                    // (pieces that never complete are given up after an hour)
                    st.parts.retain(|_, v| now() - v.started < 3600);
                    return false;
                }
                let part = st.parts.remove(&key).unwrap();
                let bytes: Vec<u8> = part.pieces.into_iter().flatten().flatten().collect();
                if bytes.len() > MAX_PICTURE * 2 || image::guess_format(&bytes).is_err() {
                    return false;
                }
                let mid = new_id();
                if crate::write_atomic(&self.media_path(&mid), &bytes).is_err() {
                    return false;
                }
                let f = &part.first;
                let pic = Picture { mid, w: f["w"].as_u64().unwrap_or(0) as u32, h: f["h"].as_u64().unwrap_or(0) as u32, once: f["once"].as_bool().unwrap_or(false), opened: false };
                let text = clip(f["text"].as_str().unwrap_or(""), 1000);
                add_msg(st, &cid, Msg { id, from: from.into(), nick, at: part.at, text, pic: Some(pic), mine, state: if mine { "sent".into() } else { "received".into() }, via: via.into(), ..Default::default() })
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
        if let (Some(o), Some(e)) = (p.as_object_mut(), extra.as_object()) {
            for (k, v) in e {
                o.insert(k.clone(), v.clone());
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
                out.push(room_event(&st.key, tag, &key, &p.to_string()));
            }
        } else {
            if !is_pubkey(cid) {
                return Err("No such chat".into());
            }
            for p in payloads {
                out.push(wrap(&st.key, cid, &p.to_string())?);
                if copy {
                    let mut c = p.clone();
                    c["to"] = json!(cid);
                    out.push(wrap(&st.key, &st.me, &c.to_string())?);
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
            let p = self.payload(&st, "msg", json!({ "id": id, "text": text, "reply": reply }));
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
        let id = new_id();
        let mid = new_id();
        crate::write_atomic(&self.media_path(&mid), &jpeg)?;
        let size = if cid.starts_with("r:") { PIECE_ROOM } else { PIECE_DM };
        let pieces: Vec<&[u8]> = jpeg.chunks(size).collect();
        let n = pieces.len();
        let events = {
            let mut st = self.st.lock().unwrap();
            let payloads: Vec<Value> = pieces.iter().enumerate().map(|(i, d)| {
                let mut extra = json!({ "id": id, "n": n, "i": i, "d": base64::engine::general_purpose::STANDARD.encode(d) });
                if i == 0 {
                    extra["w"] = json!(w);
                    extra["h"] = json!(h);
                    extra["once"] = json!(once);
                    extra["text"] = json!(clip(text, 1000));
                }
                self.payload(&st, "img", extra)
            }).collect();
            // (no copy for your other devices: a picture is big, and theirs would arrive anyway in a room)
            let events = self.seal_for(&st, cid, &payloads, false)?;
            let (me, nick) = (st.me.clone(), st.saved.nick.clone());
            accept(&mut st, cid);
            let pic = Picture { mid, w, h, once, opened: false };
            add_msg(&mut st, cid, Msg { id: id.clone(), from: me, nick, at: now_ms(), text: clip(text, 1000), pic: Some(pic), mine: true, state: "sending".into(), via: "net".into(), ..Default::default() });
            events
        };
        self.publish(cid, &id, events);
        self.bump();
        Ok(json!(id))
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
        let st = self.st.lock().unwrap();
        let ups = self.up.lock().unwrap().clone();
        // (not relay_list(): that locks the state, which is held here)
        let relays: Vec<Value> = relays_or_override(st.saved.relays.clone()).iter().map(|u| json!({ "url": u, "up": ups.get(u).copied().unwrap_or(false) })).collect();
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
                    (if c.nick.is_empty() { format!("Someone {}", &cid[..6]) } else { c.nick.clone() }, "dm", json!({ "fav": c.fav, "request": c.request }))
                }
            };
            convs.push(json!({ "id": cid, "kind": kind, "name": name, "pinned": meta.pinned, "muted": meta.muted, "unread": meta.unread,
                               "at": meta.last_at, "last": meta.last, "extra": extra }));
        }
        let blocked: Vec<Value> = st.saved.contacts.iter().filter(|(_, c)| c.blocked).map(|(k, c)| json!({ "id": k, "nick": c.nick })).collect();
        json!({
            "rev": self.rev.load(Ordering::Relaxed),
            "me": { "id": st.me, "nick": st.saved.nick, "code": format!("{CODE_PREFIX}{}", st.me) },
            "relays": relays,
            "custom": std::env::var("LV_CHAT_RELAYS").is_ok(),
            "convs": convs,
            "blocked": blocked,
            "outbox": st.saved.outbox.len(),
        })
    }

    fn messages(&self, cid: &str) -> Value {
        let mut st = self.st.lock().unwrap();
        load_conv(self, &mut st, cid);
        st.open = cid.to_string();
        if let Some(m) = st.saved.meta.get_mut(cid) {
            if m.unread > 0 {
                m.unread = 0;
                st.dirty_state = true;
            }
        }
        let list = st.msgs.get(cid).cloned().unwrap_or_default();
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

/// The relays to use (tests and the screenshot harness point it at a relay of their own).
fn relays_or_override(saved: Vec<String>) -> Vec<String> {
    match std::env::var("LV_CHAT_RELAYS") {
        Ok(list) => list.split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect(),
        Err(_) => saved,
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
        let list: Vec<Msg> = read_json(&chat.conv_path(cid));
        st.msgs.insert(cid.to_string(), list);
    }
}

fn add_msg(st: &mut St, cid: &str, msg: Msg) -> bool {
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
    let preview = if msg.pic.is_some() { if msg.text.is_empty() { "📷 Picture".to_string() } else { format!("📷 {}", msg.text) } } else { msg.text.clone() };
    let preview = if cid.starts_with("r:") && !msg.mine && !msg.nick.is_empty() { format!("{}: {}", msg.nick, preview) } else { preview };
    let (mine, at) = (msg.mine, msg.at);
    // (in time order: a late arrival goes where it belongs)
    let pos = list.iter().rposition(|m| m.at <= at).map(|p| p + 1).unwrap_or(0);
    list.insert(pos, msg);
    if list.len() > KEEP_MESSAGES {
        let extra = list.len() - KEEP_MESSAGES;
        list.drain(..extra);
    }
    st.dirty.insert(cid.to_string());
    let open = st.open == cid;
    let m = st.saved.meta.entry(cid.to_string()).or_default();
    if at >= m.last_at {
        m.last_at = at;
        m.last = clip(&preview, 120);
    }
    m.hidden = false;
    if !mine && !open {
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

// ---------- one relay ----------

fn relay_loop(chat: Arc<Chat>, url: String, rx: mpsc::Receiver<String>, stop: Arc<AtomicBool>) {
    let mut wait = 2u64;
    while !stop.load(Ordering::Relaxed) {
        let started = std::time::Instant::now();
        let r = relay_session(&chat, &url, &rx, &stop);
        chat.up.lock().unwrap().insert(url.clone(), false);
        chat.bump();
        if stop.load(Ordering::Relaxed) {
            break;
        }
        if let Err(e) = r {
            eprintln!("chat relay {url}: {e}");
        }
        // (a session that lasted a while starts the backoff over)
        if started.elapsed() > Duration::from_secs(60) {
            wait = 2;
        }
        let until = std::time::Instant::now() + Duration::from_secs(wait);
        while std::time::Instant::now() < until && !stop.load(Ordering::Relaxed) {
            std::thread::sleep(Duration::from_millis(250));
        }
        wait = (wait * 2).min(120);
    }
}

fn relay_session(chat: &Arc<Chat>, url: &str, rx: &mpsc::Receiver<String>, stop: &AtomicBool) -> Res<()> {
    use std::net::{TcpStream, ToSocketAddrs};
    use tungstenite::Message;
    let uri: tungstenite::http::Uri = url.parse().map_err(|_| "bad address")?;
    let host = uri.host().ok_or("bad address")?.to_string();
    let port = uri.port_u16().unwrap_or(if uri.scheme_str() == Some("ws") { 80 } else { 443 });
    let addr = (host.as_str(), port).to_socket_addrs().map_err(|e| e.to_string())?.next().ok_or("no address")?;
    let tcp = TcpStream::connect_timeout(&addr, Duration::from_secs(10)).map_err(|e| e.to_string())?;
    tcp.set_read_timeout(Some(Duration::from_secs(15))).ok();
    tcp.set_write_timeout(Some(Duration::from_secs(15))).ok();
    let (mut ws, _) = tungstenite::client_tls_with_config(url, tcp.try_clone().map_err(|e| e.to_string())?, None, None).map_err(|e| e.to_string())?;
    tcp.set_read_timeout(Some(Duration::from_millis(200))).ok();
    chat.up.lock().unwrap().insert(url.to_string(), true);
    chat.bump();
    let send = |ws: &mut tungstenite::WebSocket<_>, s: String| ws.send(Message::text(s)).map_err(|e: tungstenite::Error| e.to_string());
    let mut gen = 0u64;
    let mut last_ping = std::time::Instant::now();
    let mut auth_id = String::new();
    for frame in chat.outbox_events() {
        send(&mut ws, frame)?;
    }
    while !stop.load(Ordering::Relaxed) {
        let g = chat.sub_gen.load(Ordering::Relaxed);
        if g != gen {
            gen = g;
            let mut req = vec![json!("REQ"), json!("lv")];
            req.extend(chat.filters());
            send(&mut ws, json!(req).to_string())?;
        }
        // what we have to send (spaced out a little: relays limit bursts)
        let mut sent = 0;
        while let Ok(frame) = rx.try_recv() {
            send(&mut ws, frame)?;
            sent += 1;
            if sent >= 8 {
                std::thread::sleep(Duration::from_millis(120));
                sent = 0;
            }
        }
        if last_ping.elapsed() > Duration::from_secs(45) {
            ws.send(Message::Ping(Vec::new().into())).map_err(|e| e.to_string())?;
            last_ping = std::time::Instant::now();
        }
        match ws.read() {
            Ok(Message::Text(t)) => {
                let Ok(v) = serde_json::from_str::<Value>(t.as_str()) else { continue };
                match v[0].as_str().unwrap_or("") {
                    "EVENT" => {
                        if let Ok(ev) = serde_json::from_value::<Event>(v[2].clone()) {
                            chat.take(ev, "net");
                        }
                    }
                    "OK" => {
                        let id = v[1].as_str().unwrap_or("");
                        let ok = v[2].as_bool().unwrap_or(false);
                        if id == auth_id {
                            gen = 0; // (signed in: ask again)
                        } else if ok || v[3].as_str().unwrap_or("").starts_with("duplicate") {
                            chat.relay_ok(id);
                        }
                    }
                    // NIP-42: some relays only hand over wraps to the one they are for
                    "AUTH" => {
                        let challenge = v[1].as_str().unwrap_or("").to_string();
                        let st = chat.st.lock().unwrap();
                        let ev = Event::new(st.me.clone(), now(), KIND_AUTH, vec![vec!["relay".into(), url.into()], vec!["challenge".into(), challenge]], String::new()).sign(&st.key);
                        drop(st);
                        auth_id = ev.id.clone();
                        send(&mut ws, json!(["AUTH", ev]).to_string())?;
                    }
                    _ => {}
                }
            }
            Ok(Message::Close(_)) => return Err("closed".into()),
            Ok(_) => {}
            Err(tungstenite::Error::Io(e)) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {
                let _ = ws.flush();
            }
            Err(e) => return Err(e.to_string()),
        }
    }
    let _ = ws.close(None);
    Ok(())
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
        "chatMessages" => Ok(chat.messages(&cid)),
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
            let list: Vec<String> = a(0).as_array().cloned().unwrap_or_default().iter().filter_map(|v| v.as_str())
                .map(|u| u.trim().to_string()).filter(|u| u.starts_with("wss://") || u.starts_with("ws://")).take(12).collect();
            {
                let mut st = chat.st.lock().unwrap();
                st.saved.relays = if list.is_empty() { DEFAULT_RELAYS.iter().map(|s| s.to_string()).collect() } else { list };
                st.dirty_state = true;
            }
            chat.up.lock().unwrap().clear();
            chat.connect();
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
            chat.connect();
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

/// /chat/<id>: a picture in a chat (for <img>).
pub fn media(mid: &str) -> Option<Vec<u8>> {
    CHAT.lock().unwrap().as_ref().and_then(|c| c.media(mid))
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
            c.st.lock().unwrap().saved.relays = vec![format!("ws://127.0.0.1:{port}")];
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
                "msgs" => c.messages(&cid),
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
        assert!(wait(&|| a.st.lock().unwrap().msgs[&pb].iter().any(|m| m.pic.is_some())), "Alice got the picture");
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
        // the files are written, and come back
        a.flush();
        let again = Chat::open(da.clone());
        assert_eq!(again.st.lock().unwrap().me, pa);
        a.shutdown();
        b.shutdown();
        let _ = fs::remove_dir_all(&base);
    }

    /// The page's calls, through the real entry point (each under a timeout: a lock taken twice would hang).
    #[test]
    fn page_calls_never_hang() {
        let dir = std::env::temp_dir().join(format!("lv-chat-api-{}", new_id()));
        fs::create_dir_all(&dir).unwrap();
        // (a relay nobody listens on: no network in tests)
        fs::write(dir.join("state.json"), json!({ "relays": ["ws://127.0.0.1:9"] }).to_string()).unwrap();
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
        assert_eq!(st["relays"][0]["url"], "ws://127.0.0.1:9");
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
        use std::net::TcpListener;
        let listener = TcpListener::bind(("127.0.0.1", port)).unwrap();
        let port = listener.local_addr().unwrap().port();
        let store: Arc<Mutex<Vec<Event>>> = Arc::new(Mutex::new(Vec::new()));
        let clients: Arc<Mutex<Vec<mpsc::Sender<Event>>>> = Arc::new(Mutex::new(Vec::new()));
        std::thread::spawn(move || {
            for stream in listener.incoming().flatten() {
                let (store, clients) = (store.clone(), clients.clone());
                std::thread::spawn(move || {
                    let Ok(mut ws) = tungstenite::accept(stream.try_clone().unwrap()) else { return };
                    stream.set_read_timeout(Some(Duration::from_millis(50))).ok();
                    let (tx, rx) = mpsc::channel::<Event>();
                    clients.lock().unwrap().push(tx);
                    let mut subs: Vec<Value> = Vec::new();
                    let matches = |f: &Value, e: &Event| {
                        f["kinds"].as_array().map(|k| k.iter().any(|x| x.as_u64() == Some(e.kind as u64))).unwrap_or(true)
                            && f["#p"].as_array().map(|p| p.iter().any(|x| Some(x.as_str().unwrap_or("")) == e.tag("p"))).unwrap_or(true)
                            && f["#r"].as_array().map(|p| p.iter().any(|x| Some(x.as_str().unwrap_or("")) == e.tag("r"))).unwrap_or(true)
                            && f["since"].as_i64().map(|s| e.created_at >= s).unwrap_or(true)
                    };
                    loop {
                        while let Ok(e) = rx.try_recv() {
                            if subs.iter().any(|f| matches(f, &e)) {
                                if ws.send(tungstenite::Message::text(json!(["EVENT", "lv", e]).to_string())).is_err() { return; }
                            }
                        }
                        match ws.read() {
                            Ok(tungstenite::Message::Text(t)) => {
                                let v: Value = serde_json::from_str(t.as_str()).unwrap_or(Value::Null);
                                match v[0].as_str() {
                                    Some("EVENT") => {
                                        let e: Event = serde_json::from_value(v[1].clone()).unwrap();
                                        let ok = e.verify();
                                        let _ = ws.send(tungstenite::Message::text(json!(["OK", e.id, ok, ""]).to_string()));
                                        if ok {
                                            store.lock().unwrap().push(e.clone());
                                            for c in clients.lock().unwrap().iter() { let _ = c.send(e.clone()); }
                                        }
                                    }
                                    Some("REQ") => {
                                        subs = v.as_array().unwrap()[2..].to_vec();
                                        for e in store.lock().unwrap().iter() {
                                            if subs.iter().any(|f| matches(f, e)) {
                                                let _ = ws.send(tungstenite::Message::text(json!(["EVENT", "lv", e]).to_string()));
                                            }
                                        }
                                        let _ = ws.send(tungstenite::Message::text(json!(["EOSE", "lv"]).to_string()));
                                    }
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
