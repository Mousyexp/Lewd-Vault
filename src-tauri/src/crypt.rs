// Untraceable mode: the vault, encrypted, inside the app's own folder (welcome screen → Untraceable).
//
// For a PC that isn't only yours (a work laptop): the vault lives in a hidden "data.temp" folder (next
// to the app for the portable copy, in Documents for the installed one, or wherever you pick), every file in it is encrypted with a key made from your password, and nothing else goes
// anywhere on the PC: the thumbnail and web caches, unfinished uploads, the Google Drive sign-in and
// even the built-in browser's own data (WebView2, in private mode) stay inside data.temp too, and
// deleted files go to the vault's own _trash rather than the Recycle Bin. Without the password the
// files are noise: Explorer, Windows Search and photo apps can't show or index them.
//
// The format: "LVX1", a random 12-byte nonce, then the bytes XORed with ChaCha20's keystream from
// that nonce. A stream cipher, so any byte range can be read without the rest (video seeking, the
// thumbnail packs), and a file can be written in pieces (uploads). The key is PBKDF2-HMAC-SHA256 of
// the password; its salt is fixed so the same password gives the same key on every PC (a vault
// synced through Google Drive opens anywhere with it). It hides the files from anyone looking; it
// isn't tamper-proof (no authentication), which nobody here needs.
//
// With no key set (the normal modes), every function here just reads and writes plain files, so the
// rest of the app calls these everywhere and works the same either way. A plain file in an
// untraceable vault (no "LVX1" at the start) is still read as it is.

use chacha20::cipher::{KeyIvInit, StreamCipher, StreamCipherSeek};
use chacha20::ChaCha20;
use std::fs;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
#[cfg(not(test))]
use std::sync::RwLock;

const MAGIC: &[u8; 4] = b"LVX1";
const HEAD: u64 = 16;
const SALT: &[u8] = b"lewd-vault/untraceable/v1";
const ROUNDS: u32 = 310_000;
/// What the password check file holds, encrypted (see check_bytes).
const CHECK: &[u8] = b"lewd vault: the password is right";

#[cfg(not(test))]
static KEY: RwLock<Option<[u8; 32]>> = RwLock::new(None);
#[cfg(test)]
thread_local! {
    // (tests run side by side: each keeps its own key)
    static KEY: std::cell::Cell<Option<[u8; 32]>> = const { std::cell::Cell::new(None) };
}
static UNTRACEABLE: AtomicBool = AtomicBool::new(false);

pub fn untraceable() -> bool {
    UNTRACEABLE.load(Ordering::Relaxed)
}
pub fn set_untraceable(on: bool) {
    UNTRACEABLE.store(on, Ordering::Relaxed);
}
/// Untraceable, and the password not given yet: nothing in the vault can be read or written.
pub fn locked() -> bool {
    untraceable() && key().is_none()
}
#[cfg(not(test))]
fn key() -> Option<[u8; 32]> {
    *KEY.read().unwrap()
}
#[cfg(test)]
fn key() -> Option<[u8; 32]> {
    KEY.with(|k| k.get())
}
/// The key the vault in use is encrypted with (None: a normal vault, or still locked).
pub fn current_key() -> Option<[u8; 32]> {
    key()
}
#[cfg(not(test))]
pub fn set_key(k: Option<[u8; 32]>) {
    *KEY.write().unwrap() = k;
}
#[cfg(test)]
pub fn set_key(k: Option<[u8; 32]>) {
    KEY.with(|c| c.set(k));
}
pub fn derive(password: &str) -> [u8; 32] {
    let mut k = [0u8; 32];
    pbkdf2::pbkdf2_hmac::<sha2::Sha256>(password.as_bytes(), SALT, ROUNDS, &mut k);
    k
}

fn cipher(key: &[u8; 32], nonce: &[u8; 12], pos: u64) -> ChaCha20 {
    let mut c = ChaCha20::new(key.into(), nonce.into());
    c.seek(pos);
    c
}
fn new_nonce() -> [u8; 12] {
    let mut n = [0u8; 12];
    if getrandom::getrandom(&mut n).is_err() {
        let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_nanos();
        n.copy_from_slice(&md5::compute(format!("{t}{}", std::process::id())).0[..12]);
    }
    n
}
fn locked_err() -> io::Error {
    io::Error::new(io::ErrorKind::PermissionDenied, "The vault is locked")
}

/// The file's nonce, if it's one of ours.
fn header(f: &mut fs::File) -> io::Result<Option<[u8; 12]>> {
    let mut h = [0u8; HEAD as usize];
    let mut got = 0;
    while got < h.len() {
        let n = f.read(&mut h[got..])?;
        if n == 0 {
            break;
        }
        got += n;
    }
    f.seek(SeekFrom::Start(0))?;
    if got == h.len() && &h[..4] == MAGIC {
        let mut n = [0u8; 12];
        n.copy_from_slice(&h[4..]);
        return Ok(Some(n));
    }
    Ok(None)
}

/// A file opened for reading: decrypted as it's read, with seeking, and len() the plain size.
pub struct Reader {
    f: fs::File,
    c: Option<ChaCha20>,
    pos: u64,
    len: u64,
}
impl Reader {
    pub fn open(path: &Path) -> io::Result<Reader> {
        let mut f = fs::File::open(path)?;
        let size = f.metadata()?.len();
        match header(&mut f)? {
            Some(nonce) => {
                let k = key().ok_or_else(locked_err)?;
                f.seek(SeekFrom::Start(HEAD))?;
                Ok(Reader { f, c: Some(cipher(&k, &nonce, 0)), pos: 0, len: size - HEAD })
            }
            None => Ok(Reader { f, c: None, pos: 0, len: size }),
        }
    }
    pub fn len(&self) -> u64 {
        self.len
    }
}
impl Read for Reader {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.f.read(buf)?;
        if let Some(c) = self.c.as_mut() {
            c.apply_keystream(&mut buf[..n]);
        }
        self.pos += n as u64;
        Ok(n)
    }
}
impl Seek for Reader {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let pos = match to {
            SeekFrom::Start(p) => p as i128,
            SeekFrom::End(d) => self.len as i128 + d as i128,
            SeekFrom::Current(d) => self.pos as i128 + d as i128,
        };
        if pos < 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "seek before the start"));
        }
        let pos = pos as u64;
        let skip = if self.c.is_some() { HEAD } else { 0 };
        self.f.seek(SeekFrom::Start(pos + skip))?;
        if let Some(c) = self.c.as_mut() {
            c.seek(pos);
        }
        self.pos = pos;
        Ok(pos)
    }
}

/// A new file for writing: encrypted when a key is set.
pub struct Writer {
    f: fs::File,
    c: Option<ChaCha20>,
    buf: Vec<u8>,
}
impl Writer {
    pub fn create(path: &Path) -> io::Result<Writer> {
        if locked() {
            return Err(locked_err());
        }
        let mut f = fs::File::create(path)?;
        let c = match key() {
            Some(k) => {
                let nonce = new_nonce();
                f.write_all(MAGIC)?;
                f.write_all(&nonce)?;
                Some(cipher(&k, &nonce, 0))
            }
            None => None,
        };
        Ok(Writer { f, c, buf: Vec::new() })
    }
}
impl Write for Writer {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        match self.c.as_mut() {
            Some(c) => {
                self.buf.clear();
                self.buf.extend_from_slice(data);
                c.apply_keystream(&mut self.buf);
                self.f.write_all(&self.buf)?;
                Ok(data.len())
            }
            None => self.f.write(data),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        self.f.flush()?;
        // push the bytes to the storage device before the caller renames this temp file into place:
        // on flash (iOS) a rename can otherwise land while the data behind it is still only cached
        self.f.sync_all()
    }
}

pub fn read(path: &Path) -> io::Result<Vec<u8>> {
    let mut r = Reader::open(path)?;
    let mut out = Vec::with_capacity(r.len() as usize);
    r.read_to_end(&mut out)?;
    Ok(out)
}
pub fn read_to_string(path: &Path) -> io::Result<String> {
    String::from_utf8(read(path)?).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}
pub fn write(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let mut w = Writer::create(path)?;
    w.write_all(bytes)?;
    w.flush()
}
/// Copies a file, reading it decrypted (with the vault's key), and writing it encrypted with
/// `encrypt` or plain: into an untraceable vault (its key, which needn't be the one in use: moving
/// a normal vault into a new untraceable one), or out of one (None).
pub fn copy(from: &Path, to: &Path, encrypt: Option<&[u8; 32]>) -> io::Result<u64> {
    let mut r = Reader::open(from)?;
    let mut f = fs::File::create(to)?;
    let mut c = match encrypt {
        Some(k) => {
            let nonce = new_nonce();
            f.write_all(MAGIC)?;
            f.write_all(&nonce)?;
            Some(cipher(&k, &nonce, 0))
        }
        None => None,
    };
    let mut buf = vec![0u8; 1024 * 1024];
    let mut total = 0;
    loop {
        let n = r.read(&mut buf)?;
        if n == 0 {
            break;
        }
        if let Some(c) = c.as_mut() {
            c.apply_keystream(&mut buf[..n]);
        }
        f.write_all(&buf[..n])?;
        total += n as u64;
    }
    f.flush()?;
    Ok(total)
}

/// One piece of an upload: the first piece starts the file; `pos` puts a piece at that place in the
/// (plain) file, otherwise it goes on the end.
pub fn write_piece(path: &Path, first: bool, pos: Option<u64>, body: &mut dyn Read) -> io::Result<()> {
    if locked() {
        return Err(locked_err());
    }
    if first || !path.exists() {
        drop(Writer::create(path)?); // (just the header, when encrypting)
    }
    let mut f = fs::OpenOptions::new().read(true).write(true).open(path)?;
    let nonce = header(&mut f)?;
    let skip = if nonce.is_some() { HEAD } else { 0 };
    let at = match pos {
        Some(p) => p,
        None => f.metadata()?.len() - skip,
    };
    f.seek(SeekFrom::Start(at + skip))?;
    let mut c = match nonce {
        Some(n) => Some(cipher(&key().ok_or_else(locked_err)?, &n, at)),
        None => None,
    };
    let mut buf = vec![0u8; 1024 * 1024];
    loop {
        let n = body.read(&mut buf)?;
        if n == 0 {
            break;
        }
        if let Some(c) = c.as_mut() {
            c.apply_keystream(&mut buf[..n]);
        }
        f.write_all(&buf[..n])?;
    }
    Ok(())
}

/// Encrypts a plain file where it is (a file that came in plain, e.g. from Drive); nothing when it
/// already is, or there's no key.
pub fn seal(path: &Path) -> io::Result<()> {
    if key().is_none() {
        return Ok(());
    }
    let mut f = fs::File::open(path)?;
    if header(&mut f)?.is_some() {
        return Ok(());
    }
    drop(f);
    let tmp = path.with_extension("seal.part");
    let k = key();
    copy(path, &tmp, k.as_ref())?;
    fs::rename(&tmp, path)
}

/// Deleting a file: the Recycle Bin normally; for good when untraceable (the bin is a trace).
pub fn trash(path: &Path) -> bool {
    if untraceable() {
        return fs::remove_file(path).is_ok();
    }
    super::recycle::delete(path).is_ok()
}

/// The password check (the vault's _app/key.check): CHECK encrypted with a nonce of zeros, so the
/// same password always makes the same bytes (two PCs syncing don't see a change). None: no key.
pub fn check_bytes() -> Option<Vec<u8>> {
    Some(check_bytes_for(&key()?))
}
pub fn check_bytes_for(k: &[u8; 32]) -> Vec<u8> {
    let mut body = CHECK.to_vec();
    cipher(k, &[0u8; 12], 0).apply_keystream(&mut body);
    let mut out = MAGIC.to_vec();
    out.extend_from_slice(&[0u8; 12]);
    out.extend_from_slice(&body);
    out
}
/// Whether `key` opens a check file's bytes.
pub fn check_ok(key: &[u8; 32], bytes: &[u8]) -> bool {
    if bytes.len() < HEAD as usize || &bytes[..4] != MAGIC {
        return false;
    }
    let mut nonce = [0u8; 12];
    nonce.copy_from_slice(&bytes[4..16]);
    let mut body = bytes[16..].to_vec();
    cipher(key, &nonce, 0).apply_keystream(&mut body);
    body == CHECK
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips() {
        let dir = std::env::temp_dir().join(format!("lv-crypt-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let data: Vec<u8> = (0..300_000u32).map(|i| (i * 7 % 251) as u8).collect();
        // plain without a key
        write(&dir.join("p"), &data).unwrap();
        assert_eq!(fs::read(dir.join("p")).unwrap(), data);
        set_key(Some(derive("hunter2")));
        write(&dir.join("e"), &data).unwrap();
        let raw = fs::read(dir.join("e")).unwrap();
        assert_eq!(&raw[..4], MAGIC);
        assert_ne!(&raw[16..1000], &data[..984]);
        assert_eq!(read(&dir.join("e")).unwrap(), data);
        assert_eq!(read(&dir.join("p")).unwrap(), data); // plain files still read
        // ranges
        let mut r = Reader::open(&dir.join("e")).unwrap();
        assert_eq!(r.len(), data.len() as u64);
        let mut piece = vec![0u8; 5000];
        r.seek(SeekFrom::Start(123_457)).unwrap();
        r.read_exact(&mut piece).unwrap();
        assert_eq!(piece, &data[123_457..128_457]);
        // pieces, in order and at a place
        write_piece(&dir.join("u"), true, None, &mut &data[..100_000]).unwrap();
        write_piece(&dir.join("u"), false, None, &mut &data[100_000..]).unwrap();
        write_piece(&dir.join("u"), false, Some(10), &mut &data[10..20]).unwrap();
        assert_eq!(read(&dir.join("u")).unwrap(), data);
        // copies in and out, sealing
        let k = derive("hunter2");
        copy(&dir.join("p"), &dir.join("in"), Some(&k)).unwrap();
        assert_eq!(&fs::read(dir.join("in")).unwrap()[..4], MAGIC);
        copy(&dir.join("in"), &dir.join("out"), None).unwrap();
        assert_eq!(fs::read(dir.join("out")).unwrap(), data);
        seal(&dir.join("out")).unwrap();
        assert_eq!(read(&dir.join("out")).unwrap(), data);
        // the check
        let c = check_bytes().unwrap();
        assert_eq!(c, check_bytes().unwrap());
        assert!(check_ok(&derive("hunter2"), &c) && !check_ok(&derive("hunter3"), &c));
        set_key(None);
        assert!(read(&dir.join("e")).is_err()); // encrypted, no key
        let _ = fs::remove_dir_all(&dir);
    }
}
