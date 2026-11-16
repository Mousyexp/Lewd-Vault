// Lewd Vault Chat: the relays, chosen and switched by themselves.
//
// There are many free public Nostr relays and any of them can be slow, full, down or refusing on a
// given day. So the chat doesn't depend on a fixed few: it keeps a pool of every relay it knows
// (a built-in list, plus the public lists below fetched once a day), measures them itself, and keeps
// the best five linked:
//
//   scores      how fast a relay answers (connect to the end of its first answer, smoothed), how
//               often it works, and whether it takes what we publish (some only take paying users)
//   racing      when links are missing, a few extra candidates connect at once and the slowest are
//               let go as soon as enough are up: a dead relay is replaced in about a second
//   backoff     a relay that fails rests for 5 s, 10 s, 20 s... up to an hour before it is tried again
//   rotation    every few minutes a clearly faster relay takes the place of the slowest link
//   probing     in the background, relays from the lists nobody has tried yet are tested lightly, one
//               at a time: their public info document (NIP-11: paid? sign-in only?) and a handshake
//               with an empty subscription (REQ, limit 0). Nothing is published, so no spam filter
//               is bothered; whether a relay takes what we write is learned from real sends
//   trust       encrypted connections only (wss://, the certificate checked against the built-in
//               roots); every event a relay hands over is checked (its id and signature): a relay
//               that alters one, or sends what wasn't asked for, is marked, and after three is
//               dropped for good; one that keeps dropping connections ranks lower
//
// Switching relays freely could leave two people on different ones. Three things keep them meeting:
//   - two of the five links are always "core" relays (the big four), which nearly everyone shares;
//   - each user announces the relays they read on (NIP-17's kind 10050), and a private message goes
//     to the recipient's announced relays too (a short one-off connection when they aren't linked);
//   - a room meets on two core relays chosen by the room's tag, which every member works out alike.
// With your own relay list (Chat settings) none of this runs: exactly your relays are used.
use super::*;
use std::collections::BTreeSet;
use std::sync::atomic::{AtomicU32, AtomicU8};
use std::time::Instant;

/// The big four: two of them are always linked.
pub(super) const CORE: &[&str] = &["wss://relay.damus.io", "wss://nos.lol", "wss://relay.primal.net", "wss://offchain.pub"];
/// More free public relays known to take private messages (the pool learns the rest from the lists).
pub(super) const MORE: &[&str] = &[
    "wss://relay.0xchat.com", "wss://nostr.mom", "wss://relay.nostr.net", "wss://nostr.oxtr.dev", "wss://relay.snort.social",
    "wss://nostr-pub.wellorder.net", "wss://relay.nos.social", "wss://nostr.bitcoiner.social", "wss://relay.nostr.bg", "wss://nostr21.com",
    "wss://relay.nostromo.social", "wss://nostr.einundzwanzig.space", "wss://nostr.data.haus", "wss://relay.fountain.fm",
];
/// Public lists of working relays, refreshed daily (each line or row starts with an address).
const LISTS: &[&str] = &[
    "https://raw.githubusercontent.com/permissionlesstech/georelays/main/nostr_relays.csv",
    "https://raw.githubusercontent.com/sesseor/nostr-relays-list/main/relays.txt",
];
const WANT: usize = 5;
const CORE_SLOTS: usize = 2;
const RACE: usize = 3;
const MAX_KNOWN: usize = 400;
const ROTATE_EVERY: Duration = Duration::from_secs(240);
const PROBE_EVERY: Duration = Duration::from_secs(40);

const CONNECTING: u8 = 0;
const UP: u8 = 1;
const DEAD: u8 = 2;

/// What we've measured about one relay.
#[derive(Serialize, Deserialize, Clone, Default, Debug)]
#[serde(default)]
pub(super) struct Stat {
    /// milliseconds from connecting to the end of its first answer (smoothed); 0: never measured
    pub rtt: u32,
    pub ok: u32,
    pub fail: u32,
    /// failures in a row (the backoff)
    pub streak: u32,
    pub last_ok: i64,
    pub last_fail: i64,
    /// what it did with what we published: took it, or refused it
    pub accepted: u32,
    pub rejects: u32,
    /// milliseconds from publishing to its "OK" (smoothed)
    pub ack: u32,
    /// connections it dropped once up, and events it handed over altered or unasked for
    pub drops: u32,
    pub bad: u32,
    /// its info document says it wants payment or a sign-in to publish (NIP-11), and when we read it
    pub paid: bool,
    pub info_at: i64,
    /// built-in, list, yours
    pub src: String,
}

impl Stat {
    /// Altered events three times: never used again.
    pub fn banned(&self) -> bool {
        self.bad >= 3
    }
    pub fn cooling(&self, t: i64) -> bool {
        self.banned() || (self.streak > 0 && t - self.last_fail < (5i64 << self.streak.min(10)).min(3600))
    }
    pub fn score(&self) -> f64 {
        if self.banned() {
            return 0.0;
        }
        let reliable = (self.ok as f64 + 1.0) / ((self.ok + self.fail) as f64 + 2.0);
        // (dropping connections: each drop per successful connection counts against it)
        let steady = 1.0 / (1.0 + self.drops as f64 / (self.ok as f64 + 1.0));
        let ms = self.rtt.max(self.ack);
        let speed = if ms == 0 { 0.55 } else { 1.0 / (1.0 + ms as f64 / 500.0) };
        // (one that never takes what we publish is no use for sending)
        let takes = if self.rejects >= 2 && self.accepted == 0 { 0.05 } else if self.rejects > self.accepted * 2 + 2 { 0.4 } else { 1.0 };
        let paid = if self.paid && self.accepted == 0 { 0.02 } else { 1.0 };
        let honest = if self.bad > 0 { 0.3 } else { 1.0 };
        let known = if self.src == "list" && self.ok == 0 { 0.7 } else { 1.0 };
        reliable * steady * speed * takes * paid * honest * known
    }
    pub fn working(&self, t: i64) -> bool {
        self.ok > 0 && !self.cooling(t) && !(self.rejects >= 2 && self.accepted == 0) && !(self.paid && self.accepted == 0)
    }
}

/// A relay someone reads on (their kind 10050), as last seen.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub(super) struct Inbox {
    pub relays: Vec<String>,
    pub created: i64,
    pub fetched: i64,
}

/// A live connection to one relay (its thread sends and reads; this is the handle).
pub(super) struct Link {
    pub url: String,
    pub tx: mpsc::Sender<String>,
    pub stop: Arc<AtomicBool>,
    pub state: Arc<AtomicU8>,
    pub rtt: Arc<AtomicU32>,
    /// kept whatever its speed: your own list, or a room's meeting place
    pub pinned: bool,
}

impl Link {
    pub fn up(&self) -> bool {
        self.state.load(Ordering::Relaxed) == UP
    }
}

/// The two core relays a room meets on (every member works out the same two).
pub(super) fn room_relays(tag: &str) -> Vec<String> {
    let mut c: Vec<&str> = CORE.to_vec();
    c.sort_by_key(|u| sha256(format!("{tag}\n{u}").as_bytes()));
    c.into_iter().take(2).map(String::from).collect()
}

/// A relay address as the pool keeps it, or None for what we don't connect to (plain ws://, local
/// and private addresses, onion addresses).
pub(super) fn clean_url(u: &str) -> Option<String> {
    let u = u.trim().trim_end_matches('/');
    #[cfg(test)]
    if u.starts_with("ws://127.0.0.1:") {
        return Some(u.to_string()); // (the tests' own relays)
    }
    let rest = match u.split_once("://") {
        Some(("wss", r)) => r,
        Some(_) => return None,
        None => u,
    };
    let host = rest.split(['/', ':', '?', '#']).next()?.to_ascii_lowercase();
    if host.is_empty() || host == "localhost" || !host.contains('.') || host.ends_with(".local") || host.ends_with(".onion") || host.ends_with(".lan")
        || host.parse::<std::net::IpAddr>().is_ok() || rest.len() > 120 || rest.contains(char::is_whitespace) {
        return None;
    }
    let path = &rest[rest.find(['/', ':']).unwrap_or(rest.len())..];
    Some(format!("wss://{host}{path}"))
}

/// Whether we connect to an address at all: encrypted (wss://) only; plain ws:// just to this device
/// itself (the tests' relays, a relay of your own on the same machine).
pub(super) fn allowed(url: &str) -> bool {
    let Some((scheme, rest)) = url.split_once("://") else { return false };
    let host = rest.split(['/', '?', '#']).next().unwrap_or("");
    let host = host.rsplit_once(':').map(|(h, p)| if p.chars().all(|c| c.is_ascii_digit()) { h } else { host }).unwrap_or(host);
    match scheme {
        "wss" => !host.is_empty(),
        "ws" => matches!(host, "127.0.0.1" | "localhost" | "[::1]"),
        _ => false,
    }
}

fn split_list(s: &str) -> Vec<String> {
    s.split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect()
}

enum Kind {
    Live,
    Other,
}

enum Mode {
    /// a lasting link: subscriptions, everything we publish
    Live,
    /// connect, deliver these, wait for the relay to say it has them, leave
    Once(Vec<String>),
    /// connect, an empty subscription, leave: just to measure it (nothing is published)
    Probe,
}

impl Chat {
    /// Starts the pool's caretaker (once).
    pub(super) fn connect(self: &Arc<Self>) {
        if self.pool_on.swap(true, Ordering::Relaxed) {
            return;
        }
        let me = self.clone();
        std::thread::spawn(move || me.pool_loop());
    }

    /// Your own relays (Chat settings, or the tests' LV_CHAT_RELAYS): used exactly, nothing else.
    pub(super) fn manual_relays(saved: &Saved) -> Option<Vec<String>> {
        if let Ok(list) = std::env::var("LV_CHAT_RELAYS") {
            return Some(split_list(&list));
        }
        if saved.manual && !saved.relays.is_empty() {
            return Some(saved.relays.clone());
        }
        None
    }

    fn pool_loop(self: Arc<Self>) {
        let mut last_rotate = Instant::now();
        let mut last_probe = Instant::now();
        let mut last_list = Instant::now() - Duration::from_secs(3600);
        while !self.stopped.load(Ordering::Relaxed) {
            let manual = Chat::manual_relays(&self.st.lock().unwrap().saved).is_some();
            self.tend(&mut last_rotate);
            if !manual {
                if last_list.elapsed() > Duration::from_secs(3600) {
                    last_list = Instant::now();
                    self.refresh_lists();
                }
                if last_probe.elapsed() > PROBE_EVERY {
                    last_probe = Instant::now();
                    self.probe_one();
                }
            }
            self.announce_inbox();
            std::thread::sleep(Duration::from_millis(400));
        }
        for l in self.links.lock().unwrap().drain(..) {
            l.stop.store(true, Ordering::Relaxed);
        }
    }

    /// Keeps the right relays linked: what's missing connects (raced), what's surplus goes.
    fn tend(self: &Arc<Self>, last_rotate: &mut Instant) {
        let t = now();
        // (the state first, then the links: never both held, so nothing waits on the other)
        let (manual, pool, rooms) = {
            let st = self.st.lock().unwrap();
            (Chat::manual_relays(&st.saved), st.saved.pool.clone(), st.saved.rooms.keys().cloned().collect::<Vec<_>>())
        };
        let cooling = |u: &str| pool.get(u).map(|s| s.cooling(t)).unwrap_or(false);
        let mut links = self.links.lock().unwrap();
        let before = links.len();
        links.retain(|l| l.state.load(Ordering::Relaxed) != DEAD);
        let mut changed = links.len() != before;
        let linked: HashSet<String> = links.iter().map(|l| l.url.clone()).collect();
        let mut want: Vec<(String, bool)> = Vec::new();
        let add = |want: &mut Vec<(String, bool)>, u: &str, pinned: bool| {
            if !linked.contains(u) && !want.iter().any(|(w, _)| w == u) {
                want.push((u.to_string(), pinned));
            }
        };
        match manual {
            Some(list) => {
                for u in &list {
                    if !cooling(u) {
                        add(&mut want, u, true);
                    }
                }
                // (a relay no longer on your list goes)
                for l in links.iter().filter(|l| !list.contains(&l.url)) {
                    l.stop.store(true, Ordering::Relaxed);
                }
            }
            None => {
                // rooms' meeting places, kept whatever their speed
                let meet: BTreeSet<String> = rooms.iter().flat_map(|r| room_relays(r)).collect();
                for u in &meet {
                    if !cooling(u) {
                        add(&mut want, u, true);
                    }
                }
                for l in links.iter_mut() {
                    l.pinned = meet.contains(&l.url);
                }
                // a relay that turns out to refuse what we publish is no use as a link
                for l in links.iter().filter(|l| !l.pinned && l.up()) {
                    if pool.get(&l.url).map(|s| s.rejects >= 2 && s.accepted == 0).unwrap_or(false) {
                        l.stop.store(true, Ordering::Relaxed);
                        changed = true;
                    }
                }
                let free = |l: &&Link| !l.pinned;
                let up_free = links.iter().filter(free).filter(|l| l.up()).count();
                let connecting_free = links.iter().filter(free).filter(|l| !l.up()).count();
                let core_linked = links.iter().filter(|l| CORE.contains(&l.url.as_str())).count();
                let mut cands: Vec<(&String, f64)> = pool.iter().filter(|(u, s)| !linked.contains(*u) && !s.cooling(t)).map(|(u, s)| (u, s.score())).collect();
                cands.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
                // two core relays, which whoever you write to most likely reads too
                for (u, _) in cands.iter().filter(|(u, _)| CORE.contains(&u.as_str())).take(CORE_SLOTS.saturating_sub(core_linked)) {
                    add(&mut want, u, false);
                }
                // the rest, raced: a few more than needed, the slowest let go once enough are up
                if up_free < WANT {
                    let wanted = WANT - up_free + RACE;
                    for (u, _) in &cands {
                        if connecting_free + want.iter().filter(|w| !w.1).count() >= wanted {
                            break;
                        }
                        add(&mut want, u, false);
                    }
                } else {
                    // enough are up: the race is over for those still connecting
                    for l in links.iter().filter(free).filter(|l| !l.up()) {
                        l.stop.store(true, Ordering::Relaxed);
                        changed = true;
                    }
                    // and more than enough: the slowest goes (keeping two core relays)
                    let core_up = links.iter().filter(|l| l.up() && CORE.contains(&l.url.as_str())).count();
                    let ups: Vec<&Link> = links.iter().filter(free).filter(|l| l.up()).collect();
                    if ups.len() > WANT {
                        let slow = ups.iter().filter(|l| !CORE.contains(&l.url.as_str()) || core_up > CORE_SLOTS).max_by_key(|l| l.rtt.load(Ordering::Relaxed));
                        if let Some(l) = slow {
                            l.stop.store(true, Ordering::Relaxed);
                            changed = true;
                        }
                    }
                    // now and then, a clearly faster relay that works takes the slowest link's place
                    if last_rotate.elapsed() > ROTATE_EVERY {
                        *last_rotate = Instant::now();
                        let best = pool.iter().filter(|(u, s)| !linked.contains(*u) && s.working(t) && s.rtt > 0).min_by_key(|(_, s)| s.rtt);
                        let worst = ups.iter().filter(|l| !CORE.contains(&l.url.as_str())).max_by_key(|l| l.rtt.load(Ordering::Relaxed));
                        if let (Some((_, b)), Some(w)) = (best, worst) {
                            if (b.rtt as f64) < w.rtt.load(Ordering::Relaxed) as f64 * 0.5 {
                                w.stop.store(true, Ordering::Relaxed);
                                changed = true;
                            }
                        }
                    }
                }
            }
        }
        for (url, pinned) in want {
            links.push(self.spawn(url, pinned, Mode::Live));
            changed = true;
        }
        drop(links);
        if changed {
            self.bump();
        }
    }

    fn spawn(self: &Arc<Self>, url: String, pinned: bool, mode: Mode) -> Link {
        let (tx, rx) = mpsc::channel();
        let link = Link { url: url.clone(), tx, stop: Arc::new(AtomicBool::new(false)), state: Arc::new(AtomicU8::new(CONNECTING)), rtt: Arc::new(AtomicU32::new(0)), pinned };
        let (me, stop, state, rtt) = (self.clone(), link.stop.clone(), link.state.clone(), link.rtt.clone());
        let kind = match mode { Mode::Live => Kind::Live, _ => Kind::Other };
        std::thread::spawn(move || {
            let started = Instant::now();
            let r = session(&me, &url, &rx, &stop, &state, &rtt, mode);
            let was_up = state.load(Ordering::Relaxed) == UP;
            state.store(DEAD, Ordering::Relaxed);
            if !stop.load(Ordering::Relaxed) {
                // (one that never answered, or dropped us soon after, rests a while)
                if r.is_err() && (!was_up || started.elapsed() < Duration::from_secs(20)) {
                    me.note(&url, |s| { s.fail += 1; s.streak += 1; s.last_fail = now(); });
                }
                // (and a live link it dropped counts against it, however long it lasted)
                if was_up && matches!(kind, Kind::Live) {
                    me.note(&url, |s| s.drops += 1);
                }
            }
            me.bump();
        });
        link
    }

    /// Updates one relay's numbers.
    pub(super) fn note(&self, url: &str, f: impl FnOnce(&mut Stat)) {
        let mut st = self.st.lock().unwrap();
        let s = st.saved.pool.entry(url.to_string()).or_insert_with(|| Stat { src: "yours".into(), ..Default::default() });
        f(s);
        st.dirty_state = true;
    }

    /// Sends frames to one relay: through its link if it has one, or a short one-off connection.
    pub(super) fn deliver(self: &Arc<Self>, url: &str, frames: Vec<String>) {
        if frames.is_empty() {
            return;
        }
        if let Some(l) = self.links.lock().unwrap().iter().find(|l| l.url == url) {
            for f in frames {
                let _ = l.tx.send(f);
            }
            return;
        }
        let _ = self.spawn(url.to_string(), false, Mode::Once(frames));
    }

    /// Sends a frame through every link (nothing kept if none is up).
    pub(super) fn broadcast(&self, frame: &str) {
        for l in self.links.lock().unwrap().iter() {
            let _ = l.tx.send(frame.to_string());
        }
    }

    pub(super) fn links_view(&self) -> Vec<Value> {
        let mut v: Vec<Value> = self.links.lock().unwrap().iter()
            .map(|l| json!({ "url": l.url, "up": l.up(), "rtt": l.rtt.load(Ordering::Relaxed), "pinned": l.pinned, "core": CORE.contains(&l.url.as_str()) })).collect();
        v.sort_by_key(|x| (!x["up"].as_bool().unwrap_or(false), x["rtt"].as_u64().unwrap_or(0)));
        v
    }

    // ---------- meeting the people you write to ----------

    /// Announces the relays you read on (kind 10050), when they change (at most once a minute) and
    /// every few hours; to every link and to the core relays.
    fn announce_inbox(self: &Arc<Self>) {
        let ups: Vec<(String, u32)> = self.links.lock().unwrap().iter().filter(|l| l.up()).map(|l| (l.url.clone(), l.rtt.load(Ordering::Relaxed))).collect();
        if ups.len() < 2 {
            return;
        }
        let mut list: Vec<(String, u32)> = ups;
        list.sort_by_key(|(_, r)| *r);
        let mut relays: Vec<String> = list.iter().filter(|(u, _)| CORE.contains(&u.as_str())).take(2).map(|(u, _)| u.clone()).collect();
        for (u, _) in &list {
            if relays.len() >= 6 {
                break;
            }
            if !relays.contains(u) {
                relays.push(u.clone());
            }
        }
        relays.sort();
        let sig = relays.join(" ");
        let ev = {
            let mut st = self.st.lock().unwrap();
            let t = now();
            if (sig == st.saved.inbox_sig && t - st.saved.inbox_at < 6 * 3600) || t - st.saved.inbox_at < 60 {
                return;
            }
            st.saved.inbox_sig = sig;
            st.saved.inbox_at = t;
            st.dirty_state = true;
            Event::new(st.me.clone(), t, KIND_INBOX, relays.iter().map(|u| vec!["relay".into(), u.clone()]).collect(), String::new()).sign(&st.key)
        };
        let frame = json!(["EVENT", ev]).to_string();
        self.broadcast(&frame);
        let linked: HashSet<String> = self.links.lock().unwrap().iter().map(|l| l.url.clone()).collect();
        for u in CORE.iter().filter(|u| !linked.contains(**u)) {
            self.deliver(u, vec![frame.clone()]);
        }
    }

    /// Asks the links for someone's inbox relays, when we don't know them or it's been an hour.
    pub(super) fn look_up_inbox(&self, who: &str) {
        {
            let mut st = self.st.lock().unwrap();
            let t = now();
            if st.saved.inboxes.get(who).map(|i| t - i.fetched < 3600).unwrap_or(false) || st.asked_inbox.get(who).map(|a| t - a < 30).unwrap_or(false) {
                return;
            }
            st.asked_inbox.insert(who.to_string(), t);
        }
        self.broadcast(&json!(["REQ", format!("ib{}", &who[..16]), { "kinds": [KIND_INBOX], "authors": [who], "limit": 1 }]).to_string());
    }

    /// Someone's inbox relays arrived: kept, and what's waiting for them goes there too.
    pub(super) fn take_inbox(self: &Arc<Self>, ev: &Event) {
        if ev.kind != KIND_INBOX || !ev.verify() {
            return;
        }
        let relays: Vec<String> = ev.tags.iter().filter(|t| t.first().map(|s| s == "relay").unwrap_or(false)).filter_map(|t| t.get(1)).filter_map(|u| clean_url(u)).take(8).collect();
        let frames: Vec<(String, Vec<String>)> = {
            let mut st = self.st.lock().unwrap();
            let old = st.saved.inboxes.get(&ev.pubkey).cloned().unwrap_or_default();
            if ev.created_at < old.created {
                return;
            }
            let fresh = relays != old.relays;
            st.saved.inboxes.insert(ev.pubkey.clone(), Inbox { relays: relays.clone(), created: ev.created_at, fetched: now() });
            if st.saved.inboxes.len() > 300 {
                let oldest = st.saved.inboxes.iter().min_by_key(|(_, i)| i.fetched).map(|(k, _)| k.clone());
                if let Some(k) = oldest { st.saved.inboxes.remove(&k); }
            }
            st.dirty_state = true;
            if !fresh {
                return;
            }
            let waiting: Vec<String> = st.saved.outbox.iter().filter(|o| o.cid == ev.pubkey).map(|o| json!(["EVENT", o.ev]).to_string()).collect();
            relays.iter().map(|u| (u.clone(), waiting.clone())).collect()
        };
        for (u, f) in frames {
            self.deliver(&u, f);
        }
    }

    /// Where a private message to `who` goes besides the links: their announced relays.
    pub(super) fn inbox_of(&self, who: &str) -> Vec<String> {
        self.st.lock().unwrap().saved.inboxes.get(who).map(|i| i.relays.clone()).unwrap_or_default()
    }

    // ---------- the lists, and learning which relays work ----------

    /// Fetches the public relay lists (daily) and adds what's new to the pool.
    fn refresh_lists(self: &Arc<Self>) {
        if now() - self.st.lock().unwrap().saved.list_at < 20 * 3600 {
            return;
        }
        let me = self.clone();
        std::thread::spawn(move || {
            let mut found: BTreeSet<String> = BTreeSet::new();
            for src in LISTS {
                let Ok(r) = ureq::get(src).timeout(Duration::from_secs(20)).call() else { continue };
                let Ok(text) = r.into_string() else { continue };
                for line in text.lines().take(3000) {
                    let first = line.split([',', ';', '\t']).next().unwrap_or("");
                    if let Some(u) = clean_url(first) {
                        found.insert(u);
                    }
                }
            }
            let mut st = me.st.lock().unwrap();
            // (a failed fetch tries again in about two hours)
            st.saved.list_at = if found.is_empty() { now() - 18 * 3600 } else { now() };
            if !found.is_empty() {
                st.saved.list_ok = now();
            }
            for u in found {
                st.saved.pool.entry(u).or_insert_with(|| Stat { src: "list".into(), ..Default::default() });
            }
            prune(&mut st.saved.pool);
            st.dirty_state = true;
        });
    }

    /// Tests one relay nobody has tried (or not for a few days), in the background.
    fn probe_one(self: &Arc<Self>) {
        if self.probing.load(Ordering::Relaxed) {
            return;
        }
        let t = now();
        let pick = {
            let st = self.st.lock().unwrap();
            let linked: HashSet<String> = self.links.lock().unwrap().iter().map(|l| l.url.clone()).collect();
            st.saved.pool.iter()
                .filter(|(u, s)| !linked.contains(*u) && !s.cooling(t) && (s.ok + s.fail == 0 || t - s.last_ok.max(s.last_fail) > 3 * 86400))
                .min_by_key(|(_, s)| (s.ok + s.fail, s.last_ok.max(s.last_fail)))
                .map(|(u, _)| u.clone())
        };
        if let Some(u) = pick {
            self.probing.store(true, Ordering::Relaxed);
            let _ = self.spawn(u, false, Mode::Probe);
        }
    }

    /// A relay's public info document (NIP-11: plain HTTPS, nothing sent to its relay): whether it
    /// wants payment (or otherwise restricts who publishes) before taking what we send.
    fn read_info(&self, url: &str) {
        let Some(page) = url.strip_prefix("wss://").map(|r| format!("https://{r}")) else { return };
        let Ok(r) = ureq::get(&page).set("Accept", "application/nostr+json").timeout(Duration::from_secs(6)).call() else { return };
        let Ok(text) = r.into_string() else { return };
        let Ok(v) = serde_json::from_str::<Value>(&text) else { return };
        let l = &v["limitation"];
        // (a sign-in alone, NIP-42, is fine: the chat signs in)
        let paid = l["payment_required"].as_bool() == Some(true) || l["restricted_writes"].as_bool() == Some(true);
        self.note(url, |s| { s.paid = paid; s.info_at = now(); });
    }
}

/// Keeps the pool to MAX_KNOWN: what never worked from the lists goes first.
fn prune(pool: &mut BTreeMap<String, Stat>) {
    while pool.len() > MAX_KNOWN {
        let victim = pool.iter().filter(|(_, s)| s.src == "list")
            .min_by(|a, b| (a.1.ok, std::cmp::Reverse(a.1.fail)).cmp(&(b.1.ok, std::cmp::Reverse(b.1.fail))))
            .map(|(u, _)| u.clone());
        match victim {
            Some(u) => { pool.remove(&u); }
            None => break,
        }
    }
}

/// The relays a fresh pool starts with.
pub(super) fn seed(pool: &mut BTreeMap<String, Stat>) {
    for u in CORE.iter().chain(MORE) {
        let s = pool.entry(u.to_string()).or_default();
        if s.src.is_empty() || s.src == "list" {
            s.src = "built-in".into();
        }
    }
}

// ---------- one connection ----------

fn session(chat: &Arc<Chat>, url: &str, rx: &mpsc::Receiver<String>, stop: &AtomicBool, state: &AtomicU8, rtt: &AtomicU32, mode: Mode) -> Res<()> {
    use std::net::{TcpStream, ToSocketAddrs};
    use tungstenite::Message;
    let probe = matches!(mode, Mode::Probe);
    let _probing = ProbeFlag(chat.clone(), probe);
    if !allowed(url) {
        return Err("not an encrypted (wss://) address".into());
    }
    if probe {
        chat.read_info(url);
    }
    let started = Instant::now();
    let uri: tungstenite::http::Uri = url.parse().map_err(|_| "bad address")?;
    let host = uri.host().ok_or("bad address")?.to_string();
    let port = uri.port_u16().unwrap_or(if uri.scheme_str() == Some("ws") { 80 } else { 443 });
    let addr = (host.as_str(), port).to_socket_addrs().map_err(|e| e.to_string())?.next().ok_or("no address")?;
    let tcp = TcpStream::connect_timeout(&addr, Duration::from_secs(6)).map_err(|e| e.to_string())?;
    tcp.set_nodelay(true).ok();
    tcp.set_read_timeout(Some(Duration::from_secs(8))).ok();
    tcp.set_write_timeout(Some(Duration::from_secs(10))).ok();
    let (mut ws, _) = tungstenite::client_tls_with_config(url, tcp.try_clone().map_err(|e| e.to_string())?, None, tls()).map_err(|e| e.to_string())?;
    tcp.set_read_timeout(Some(Duration::from_millis(150))).ok();
    let send = |ws: &mut tungstenite::WebSocket<_>, s: String| ws.send(Message::text(s)).map_err(|e: tungstenite::Error| e.to_string());
    let mut gen = 0u64;
    let mut last_ping = Instant::now();
    let mut auth_id = String::new();
    let mut asked_at: Option<Instant> = None;
    // a one-off delivery waits for these to be acknowledged; a probe for its answer
    let mut waiting: HashSet<String> = HashSet::new();
    let mut probe_read = false;
    // when each event we published went out (how long the relay takes to say OK)
    let mut sent_at: HashMap<String, Instant> = HashMap::new();
    let me = chat.st.lock().unwrap().me.clone();
    let mut outbox_sent = false;
    match &mode {
        Mode::Once(frames) => {
            for f in frames {
                if let Some(id) = event_id(f) {
                    waiting.insert(id.clone());
                    sent_at.insert(id, Instant::now());
                }
                send(&mut ws, f.clone())?;
            }
        }
        Mode::Probe => {
            // the lightest question there is: an empty subscription, answered with "end of stored events"
            send(&mut ws, json!(["REQ", "probe", { "kinds": [KIND_WRAP], "#p": [pub_of(&new_key())], "limit": 0 }]).to_string())?;
            asked_at = Some(Instant::now());
        }
        Mode::Live => {}
    }
    let mark_up = |state: &AtomicU8, rtt: &AtomicU32| {
        if state.swap(UP, Ordering::Relaxed) == UP {
            return;
        }
        let ms = started.elapsed().as_millis().min(60_000) as u32;
        rtt.store(ms, Ordering::Relaxed);
        chat.note(url, |s| {
            s.rtt = if s.rtt == 0 { ms } else { (s.rtt * 2 + ms) / 3 };
            s.ok += 1;
            s.streak = 0;
            s.last_ok = now();
        });
        chat.bump();
    };
    while !stop.load(Ordering::Relaxed) {
        match &mode {
            Mode::Live => {
                let g = chat.sub_gen.load(Ordering::Relaxed);
                if g != gen {
                    gen = g;
                    let mut req = vec![json!("REQ"), json!("lv")];
                    req.extend(chat.filters());
                    send(&mut ws, json!(req).to_string())?;
                    if asked_at.is_none() {
                        asked_at = Some(Instant::now());
                    }
                }
                // (a relay slow to finish its first answer counts as up after 6 s)
                if state.load(Ordering::Relaxed) != UP && asked_at.map(|a| a.elapsed() > Duration::from_secs(6)).unwrap_or(false) {
                    mark_up(state, rtt);
                }
                if state.load(Ordering::Relaxed) == UP {
                    // what we have to send (spaced a little: relays limit bursts)
                    let mut sent = 0;
                    while let Ok(frame) = rx.try_recv() {
                        if let Some(id) = event_id(&frame) {
                            sent_at.insert(id, Instant::now());
                        }
                        send(&mut ws, frame)?;
                        sent += 1;
                        if sent >= 8 {
                            std::thread::sleep(Duration::from_millis(100));
                            sent = 0;
                        }
                    }
                }
                if last_ping.elapsed() > Duration::from_secs(45) {
                    ws.send(Message::Ping(Vec::new().into())).map_err(|e| e.to_string())?;
                    last_ping = Instant::now();
                }
            }
            Mode::Once(_) => {
                if waiting.is_empty() || started.elapsed() > Duration::from_secs(12) {
                    break;
                }
            }
            Mode::Probe => {
                if probe_read || started.elapsed() > Duration::from_secs(10) {
                    break;
                }
            }
        }
        match ws.read() {
            Ok(Message::Text(t)) => {
                let Ok(v) = serde_json::from_str::<Value>(t.as_str()) else { continue };
                match v[0].as_str().unwrap_or("") {
                    "EVENT" => {
                        let sub = v[1].as_str().unwrap_or("");
                        let Ok(ev) = serde_json::from_value::<Event>(v[2].clone()) else { continue };
                        // every event checked: its id and signature (an altered one fails), and that
                        // it is what we asked for
                        let asked = if sub.starts_with("ib") { ev.kind == KIND_INBOX }
                            else { sub == "lv" && (matches!(ev.kind, KIND_WRAP | KIND_WRAP_NOW) && ev.tag("p") == Some(me.as_str()) || ev.kind == KIND_ROOM) };
                        if !asked || !ev.verify() {
                            let mut banned = false;
                            chat.note(url, |s| { s.bad += 1; banned = s.banned(); });
                            if banned {
                                return Err("it altered events".into());
                            }
                            continue;
                        }
                        if sub.starts_with("ib") {
                            chat.take_inbox(&ev);
                        } else {
                            chat.take_checked(ev, "net");
                        }
                    }
                    "EOSE" => {
                        let sub = v[1].as_str().unwrap_or("");
                        if sub == "lv" || sub == "probe" {
                            if probe { probe_read = true; }
                            mark_up(state, rtt);
                            if !probe && !outbox_sent {
                                // (now up: what's waiting in the outbox goes too, once)
                                outbox_sent = true;
                                for frame in chat.outbox_events() {
                                    send(&mut ws, frame)?;
                                }
                            }
                        } else if sub.starts_with("ib") {
                            let _ = send(&mut ws, json!(["CLOSE", sub]).to_string());
                        }
                    }
                    "OK" => {
                        let id = v[1].as_str().unwrap_or("");
                        let ok = v[2].as_bool().unwrap_or(false);
                        let why = v[3].as_str().unwrap_or("");
                        if id == auth_id {
                            gen = 0; // (signed in: ask again)
                        } else {
                            waiting.remove(id);
                            let ms = sent_at.remove(id).map(|t| t.elapsed().as_millis().min(60_000) as u32);
                            if ok || why.starts_with("duplicate") {
                                chat.relay_ok(id);
                                chat.note(url, |s| {
                                    s.accepted += 1;
                                    if let Some(ms) = ms { s.ack = if s.ack == 0 { ms } else { (s.ack * 2 + ms) / 3 }; }
                                });
                            } else {
                                chat.note(url, |s| s.rejects += 1);
                            }
                        }
                    }
                    "CLOSED" => {
                        // (a relay that won't serve our subscription: no use as a link)
                        if v[1].as_str() == Some("lv") && !v[2].as_str().unwrap_or("").starts_with("auth-required") {
                            chat.note(url, |s| s.rejects += 1);
                            return Err("subscription refused".into());
                        }
                    }
                    // NIP-42: some relays only hand over wraps to the one they are for
                    "AUTH" if !probe => {
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
    if probe && !probe_read {
        let _ = ws.close(None);
        return Err("no answer".into());
    }
    let _ = ws.close(None);
    Ok(())
}

/// An EVENT frame's event id, without parsing it all ("id" is a key nothing else in our events uses).
fn event_id(frame: &str) -> Option<String> {
    if !frame.starts_with("[\"EVENT\"") {
        return None;
    }
    let at = frame.find("\"id\":\"")? + 6;
    frame.get(at..at + 64).filter(|s| s.bytes().all(|c| c.is_ascii_hexdigit())).map(String::from)
}

/// The TLS setup: the built-in roots, which check every relay's certificate (None: tungstenite's own
/// rustls with webpki's roots; there is no way here to skip the check); in tests, plus LV_TEST_CA.
#[cfg(not(test))]
fn tls() -> Option<tungstenite::Connector> {
    None
}
#[cfg(test)]
fn tls() -> Option<tungstenite::Connector> {
    use rustls_pki_types::pem::PemObject;
    let path = std::env::var("LV_TEST_CA").ok()?;
    let mut roots = rustls::RootCertStore { roots: webpki_roots::TLS_SERVER_ROOTS.to_vec() };
    for c in rustls_pki_types::CertificateDer::pem_file_iter(&path).ok()?.flatten() {
        let _ = roots.add(c);
    }
    let cfg = rustls::ClientConfig::builder().with_root_certificates(roots).with_no_client_auth();
    Some(tungstenite::Connector::Rustls(Arc::new(cfg)))
}

/// (clears "a probe is running" however the probe ends)
struct ProbeFlag(Arc<Chat>, bool);
impl Drop for ProbeFlag {
    fn drop(&mut self) {
        if self.1 {
            self.0.probing.store(false, Ordering::Relaxed);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn addresses() {
        assert_eq!(clean_url("wss://Relay.Damus.io/").as_deref(), Some("wss://relay.damus.io"));
        assert_eq!(clean_url("nos.lol").as_deref(), Some("wss://nos.lol"));
        assert_eq!(clean_url("relay.vrtmrz.net:443").as_deref(), Some("wss://relay.vrtmrz.net:443"));
        assert_eq!(clean_url("wss://relay.example.com/inbox").as_deref(), Some("wss://relay.example.com/inbox"));
        for bad in ["ws://plain.example.com", "wss://192.168.1.243", "wss://localhost:7777", "wss://abc.onion", "wss://printer.local", "https://x.com", "wss://nodot", ""] {
            assert_eq!(clean_url(bad), None, "{bad}");
        }
    }

    #[test]
    fn encrypted_only() {
        assert!(allowed("wss://relay.damus.io") && allowed("wss://relay.example.com:7777/path"));
        assert!(allowed("ws://127.0.0.1:7447") && allowed("ws://localhost:7447"));
        for bad in ["ws://relay.example.com", "ws://10.0.0.5:7000", "http://relay.example.com", "relay.example.com", "wss://"] {
            assert!(!allowed(bad), "{bad}");
        }
        // (refused before any connection is tried)
        let c = Chat::open(std::env::temp_dir().join(format!("lv-tls-{}", new_id())));
        let (_tx, rx) = mpsc::channel();
        let r = session(&c, "ws://relay.example.com", &rx, &AtomicBool::new(false), &AtomicU8::new(0), &AtomicU32::new(0), Mode::Live);
        assert!(r.unwrap_err().contains("encrypted"));
    }

    #[test]
    fn scores_and_rest() {
        let t = now();
        let fast = Stat { rtt: 120, ok: 10, accepted: 5, ..Default::default() };
        let slow = Stat { rtt: 1500, ok: 10, accepted: 5, ..Default::default() };
        let flaky = Stat { rtt: 120, ok: 2, fail: 8, ..Default::default() };
        let paid = Stat { rtt: 100, ok: 5, rejects: 3, ..Default::default() };
        let unknown = Stat::default();
        assert!(fast.score() > slow.score() && fast.score() > flaky.score() && fast.score() > paid.score());
        assert!(unknown.score() > paid.score(), "one that refuses what we publish comes after one never tried");
        let resting = Stat { streak: 3, last_fail: t - 10, ..Default::default() };
        assert!(resting.cooling(t) && !resting.cooling(t + 41));
        assert!(fast.working(t) && !paid.working(t) && !unknown.working(t));
        let dropper = Stat { drops: 20, ..fast.clone() };
        assert!(fast.score() > dropper.score() * 2.0, "one that keeps dropping connections ranks lower");
        let slow_ack = Stat { ack: 2500, ..fast.clone() };
        assert!(fast.score() > slow_ack.score(), "slow to acknowledge counts too");
        let liar = Stat { bad: 3, ..fast.clone() };
        assert!(liar.banned() && liar.cooling(t) && liar.score() == 0.0 && !liar.working(t));
        let pay = Stat { paid: true, ..unknown.clone() };
        assert!(pay.score() < unknown.score() * 0.1);
    }

    /// A probe asks one empty question and publishes nothing.
    #[test]
    fn probes_only_read() {
        let r = super::super::tests::TestRelay::start();
        let c = Chat::open(std::env::temp_dir().join(format!("lv-probe-{}", new_id())));
        c.st.lock().unwrap().saved.pool.insert(r.url(), Stat { src: "list".into(), ..Default::default() });
        c.probe_one();
        let wait = |f: &dyn Fn() -> bool| { for _ in 0..80 { if f() { return true; } std::thread::sleep(Duration::from_millis(100)); } false };
        assert!(wait(&|| c.st.lock().unwrap().saved.pool[&r.url()].ok == 1), "measured");
        assert!(c.st.lock().unwrap().saved.pool[&r.url()].rtt > 0);
        assert_eq!(r.published.load(Ordering::Relaxed), 0, "nothing published");
        assert!(wait(&|| !c.probing.load(Ordering::Relaxed)));
    }

    /// A relay that alters what it passes on: the altered events are thrown away, the relay is
    /// marked each time and dropped for good after three, and the messages still arrive (another relay).
    #[test]
    fn a_relay_that_alters_events_is_dropped() {
        use super::super::tests::TestRelay;
        let (honest, liar) = (TestRelay::start(), TestRelay::start());
        let base = std::env::temp_dir().join(format!("lv-liar-{}", new_id()));
        let (a, b) = (Chat::open(base.join("a")), Chat::open(base.join("b")));
        for c in [&a, &b] {
            let mut st = c.st.lock().unwrap();
            st.saved.pool.clear();
            st.saved.pool.insert(honest.url(), Stat { src: "test".into(), ..Default::default() });
            st.saved.pool.insert(liar.url(), Stat { src: "test".into(), ..Default::default() });
            st.saved.list_at = now();
            drop(st);
            c.connect();
        }
        let wait = |f: &dyn Fn() -> bool, secs: u64| { for _ in 0..secs * 10 { if f() { return true; } std::thread::sleep(Duration::from_millis(100)); } false };
        let ups = |c: &Chat| c.links.lock().unwrap().iter().filter(|l| l.up()).count();
        assert!(wait(&|| ups(&a) == 2 && ups(&b) == 2, 10));
        liar.tamper.store(true, Ordering::Relaxed);
        let (pa, pb) = (a.st.lock().unwrap().me.clone(), b.st.lock().unwrap().me.clone());
        a.add_contact(&pb, "Bob").unwrap();
        { let mut st = b.st.lock().unwrap(); load_conv(&b, &mut st, &pa); }
        for i in 0..4 {
            a.send_text(&pb, &format!("note {i}"), "").unwrap();
        }
        assert!(wait(&|| b.st.lock().unwrap().msgs[&pa].iter().filter(|m| m.text.starts_with("note ")).count() == 4, 10), "all four arrived");
        assert!(wait(&|| b.st.lock().unwrap().saved.pool[&liar.url()].banned(), 10), "the liar is banned");
        assert!(wait(&|| !b.links.lock().unwrap().iter().any(|l| l.url == liar.url()), 5), "and dropped");
        std::thread::sleep(Duration::from_secs(2));
        assert!(!b.links.lock().unwrap().iter().any(|l| l.url == liar.url()), "and never linked again");
        a.shutdown();
        b.shutdown();
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn rooms_meet_on_the_same_two() {
        let a = room_relays("abc");
        assert_eq!(a, room_relays("abc"));
        assert_eq!(a.len(), 2);
        assert!(a.iter().all(|u| CORE.contains(&u.as_str())));
        assert!((0..20).map(|i| room_relays(&format!("room{i}"))).collect::<BTreeSet<_>>().len() > 2, "rooms spread over the core");
    }

    #[test]
    fn the_lists_parse() {
        let csv = "relay.example.org,1.3,103.6\nnostr.example.net:443,43.6,-79.3\n";
        let txt = "wss://a.example.com\nwss://192.168.1.243\nwss://b.example.com/\n";
        let got: Vec<String> = csv.lines().chain(txt.lines()).filter_map(|l| clean_url(l.split([',', ';', '\t']).next().unwrap_or(""))).collect();
        assert_eq!(got, vec!["wss://relay.example.org", "wss://nostr.example.net:443", "wss://a.example.com", "wss://b.example.com"]);
    }

    /// The real thing, on the public relays (needs the internet; run by hand):
    /// cargo test public_relays_live -- --ignored --nocapture
    #[test]
    #[ignore]
    fn public_relays_live() {
        let base = std::env::temp_dir().join(format!("lv-live-{}", new_id()));
        let (a, b) = (Chat::open(base.join("a")), Chat::open(base.join("b")));
        let t0 = Instant::now();
        a.connect();
        b.connect();
        let ups = |c: &Chat| c.links.lock().unwrap().iter().filter(|l| l.up()).count();
        while (ups(&a) < WANT || ups(&b) < WANT) && t0.elapsed() < Duration::from_secs(30) {
            std::thread::sleep(Duration::from_millis(100));
        }
        println!("linked in {:?}: Alice {}, Bob {}", t0.elapsed(), ups(&a), ups(&b));
        for l in a.links_view() { println!("  {l}"); }
        let (pa, pb) = (a.st.lock().unwrap().me.clone(), b.st.lock().unwrap().me.clone());
        a.add_contact(&pb, "Bob").unwrap();
        { let mut st = b.st.lock().unwrap(); load_conv(&b, &mut st, &pa); }
        let t1 = Instant::now();
        a.send_text(&pb, "hello over the public relays", "").unwrap();
        while !b.st.lock().unwrap().msgs[&pa].iter().any(|m| m.text.starts_with("hello over")) && t1.elapsed() < Duration::from_secs(30) {
            std::thread::sleep(Duration::from_millis(50));
        }
        let got = b.st.lock().unwrap().msgs[&pa].iter().any(|m| m.text.starts_with("hello over"));
        println!("delivered: {got} in {:?}", t1.elapsed());
        // the lists, then a minute of probing
        a.st.lock().unwrap().saved.list_at = 0;
        std::thread::sleep(Duration::from_secs(60));
        let st = a.st.lock().unwrap();
        let t = now();
        println!("pool: {} known, {} working", st.saved.pool.len(), st.saved.pool.values().filter(|s| s.working(t)).count());
        let mut best: Vec<(&String, &Stat)> = st.saved.pool.iter().filter(|(_, s)| s.ok > 0).collect();
        best.sort_by_key(|(_, s)| s.rtt);
        for (u, s) in best.iter().take(25) { println!("  {u}: {} ms, ok {} fail {} took {} refused {} ({})", s.rtt, s.ok, s.fail, s.accepted, s.rejects, s.src); }
        drop(st);
        assert!(got);
        a.shutdown();
        b.shutdown();
        let _ = fs::remove_dir_all(&base);
    }

    /// Three relays; Alice uses the first two, Bob the last two. Their shared one goes down: Alice
    /// notices at once, and still reaches Bob through the relays he announced (his inbox).
    #[test]
    fn switching_and_meeting() {
        let (p1, _s1) = super::super::tests::test_relay_ctl();
        let (p2, s2) = super::super::tests::test_relay_ctl();
        let (p3, _s3) = super::super::tests::test_relay_ctl();
        let url = |p: u16| format!("ws://127.0.0.1:{p}");
        let base = std::env::temp_dir().join(format!("lv-pool-test-{}", new_id()));
        let (a, b) = (Chat::open(base.join("a")), Chat::open(base.join("b")));
        for (c, ps) in [(&a, [p1, p2]), (&b, [p2, p3])] {
            let mut st = c.st.lock().unwrap();
            st.saved.pool.clear();
            for p in ps {
                st.saved.pool.insert(url(p), Stat { src: "test".into(), ..Default::default() });
            }
            st.saved.list_at = now(); // (no fetching the public lists in a test)
            drop(st);
            c.connect();
        }
        let wait = |f: &dyn Fn() -> bool, secs: u64| { for _ in 0..secs * 10 { if f() { return true; } std::thread::sleep(Duration::from_millis(100)); } false };
        let ups = |c: &Chat| c.links.lock().unwrap().iter().filter(|l| l.up()).count();
        assert!(wait(&|| ups(&a) == 2 && ups(&b) == 2, 10), "both linked to their two relays");
        assert!(wait(&|| b.st.lock().unwrap().saved.inbox_at > 0, 10), "Bob announced where he reads");
        std::thread::sleep(Duration::from_millis(300)); // (his announcement on its way to the relays)
        let (pa, pb) = (a.st.lock().unwrap().me.clone(), b.st.lock().unwrap().me.clone());
        a.add_contact(&pb, "Bob").unwrap();
        { let mut st = b.st.lock().unwrap(); load_conv(&b, &mut st, &pa); }
        a.send_text(&pb, "first", "").unwrap();
        assert!(wait(&|| b.st.lock().unwrap().msgs.get(&pa).map(|m| m.iter().any(|m| m.text == "first")).unwrap_or(false), 10), "the first one through the shared relay");
        assert!(wait(&|| !a.inbox_of(&pb).is_empty(), 10), "Alice learned Bob's inbox");
        assert!(a.inbox_of(&pb).contains(&url(p3)));
        // the shared relay goes down
        s2.store(true, Ordering::Relaxed);
        assert!(wait(&|| !a.links.lock().unwrap().iter().any(|l| l.url == url(p2) && l.up()), 10), "Alice dropped the dead relay");
        a.send_text(&pb, "second", "").unwrap();
        assert!(wait(&|| b.st.lock().unwrap().msgs[&pa].iter().any(|m| m.text == "second"), 15), "the second one reached Bob through his inbox relay");
        assert!(wait(&|| a.st.lock().unwrap().saved.pool[&url(p2)].streak > 0, 15), "the dead relay is resting");
        a.shutdown();
        b.shutdown();
        let _ = fs::remove_dir_all(&base);
    }
}
