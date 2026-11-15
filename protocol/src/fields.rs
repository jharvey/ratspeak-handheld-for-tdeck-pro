//! LXMF message `fields`: building messages that carry a caller-supplied fields map, and the
//! audio field (`FIELD_AUDIO`) used for voice messages.
//!
//! The lite codec (`lxmf-lite-core`) builds messages with an empty fields map only. This module
//! packs the same `[timestamp, title, content, fields]` payload with a non-empty map and signs /
//! encrypts it exactly like the codec does, so a message without fields is byte-identical to the
//! codec's (tests pin that) and a message with fields is what Python `LXMessage.pack()` produces.
//!
//! LXMF constants (LXMF/LXMF.py): `FIELD_AUDIO = 0x07`, audio modes `AM_CODEC2_700C = 0x03`,
//! `AM_CODEC2_1600 = 0x07`, `AM_CODEC2_3200 = 0x09` (others: 0x01..0x09 Codec2, 0x10..0x19 Opus,
//! 0xFF custom). The field value is a 2-element array `[audio_mode, audio_bytes]`; for Codec2
//! (Sideband) `audio_bytes` are the raw concatenated Codec2 frames of an 8 kHz codec, no header.

use super::*;

use rns_lite_core::crypto;
use rns_lite_core::identity::{destination_hash_from_parts, identity_hash};
use rns_lite_core::wire::sha256;

pub const RS_HANDHELD_LXMF_FIELD_AUDIO: u8 = 0x07;
pub const RS_HANDHELD_LXMF_AM_CODEC2_700C: u8 = 0x03;
pub const RS_HANDHELD_LXMF_AM_CODEC2_1600: u8 = 0x07;
pub const RS_HANDHELD_LXMF_AM_CODEC2_3200: u8 = 0x09;
/// Largest audio-field header [`audio_field_header`] writes: fixmap(1) + key(1) + fixarray(1) +
/// mode (<= 2) + bin32 header (5).
pub const RS_HANDHELD_LXMF_AUDIO_HEADER_MAX: usize = 10;

/// `dest(16) || source(16) || signature(64)`.
const PACKED_PREFIX: usize = lxmf_codec::LXMF_PACKED_PREFIX;
/// `source(16) || signature(64)` — the opportunistic plaintext prefix (dest is in the RNS header).
const PLAINTEXT_PREFIX: usize = 16 + 64;
/// The 1-byte empty fixmap the codec writes when there are no fields.
const EMPTY_FIELDS: &[u8] = &[0x80];

// ---- msgpack helpers -------------------------------------------------------------------------

fn bin_header_len(len: usize) -> usize {
    if len <= u8::MAX as usize {
        2
    } else if len <= u16::MAX as usize {
        3
    } else {
        5
    }
}

fn write_bin_header(len: usize, out: &mut [u8]) -> Option<usize> {
    let n = bin_header_len(len);
    if out.len() < n {
        return None;
    }
    if n == 2 {
        out[0] = 0xc4;
        out[1] = len as u8;
    } else if n == 3 {
        out[0] = 0xc5;
        out[1..3].copy_from_slice(&(len as u16).to_be_bytes());
    } else {
        out[0] = 0xc6;
        out[1..5].copy_from_slice(&u32::try_from(len).ok()?.to_be_bytes());
    }
    Some(n)
}

fn be(data: &[u8], pos: usize, n: usize) -> Option<usize> {
    let bytes = data.get(pos..pos.checked_add(n)?)?;
    Some(bytes.iter().fold(0usize, |acc, b| (acc << 8) | *b as usize))
}

/// Position just past the msgpack value at `pos`, or `None` if it is truncated or malformed.
/// Bounded recursion (depth <= 8) and checked arithmetic: attacker lengths never index past `data`.
fn skip(data: &[u8], pos: usize, depth: u8) -> Option<usize> {
    if depth > 8 {
        return None;
    }
    let marker = *data.get(pos)?;
    let p = pos + 1;
    let span = |len: usize, at: usize| -> Option<usize> {
        let end = at.checked_add(len)?;
        (end <= data.len()).then_some(end)
    };
    let items = |count: usize, mut at: usize| -> Option<usize> {
        for _ in 0..count {
            at = skip(data, at, depth + 1)?;
        }
        Some(at)
    };
    match marker {
        0x00..=0x7f | 0xe0..=0xff | 0xc0 | 0xc2 | 0xc3 => Some(p),
        0x80..=0x8f => items(2 * (marker & 0x0f) as usize, p),
        0x90..=0x9f => items((marker & 0x0f) as usize, p),
        0xa0..=0xbf => span((marker & 0x1f) as usize, p),
        0xc4 | 0xd9 => span(be(data, p, 1)?, p + 1),
        0xc5 | 0xda => span(be(data, p, 2)?, p + 2),
        0xc6 | 0xdb => span(be(data, p, 4)?, p + 4),
        0xc7 => span(be(data, p, 1)?.checked_add(1)?, p + 1), // ext8: len, type, data
        0xc8 => span(be(data, p, 2)?.checked_add(1)?, p + 2),
        0xc9 => span(be(data, p, 4)?.checked_add(1)?, p + 4),
        0xca => span(4, p),
        0xcb => span(8, p),
        0xcc | 0xd0 => span(1, p),
        0xcd | 0xd1 => span(2, p),
        0xce | 0xd2 => span(4, p),
        0xcf | 0xd3 => span(8, p),
        0xd4 => span(2, p),
        0xd5 => span(3, p),
        0xd6 => span(5, p),
        0xd7 => span(9, p),
        0xd8 => span(17, p),
        0xdc => items(be(data, p, 2)?, p + 2),
        0xdd => items(be(data, p, 4)?, p + 4),
        0xde => items(be(data, p, 2)?.checked_mul(2)?, p + 2),
        0xdf => items(be(data, p, 4)?.checked_mul(2)?, p + 4),
        0xc1 => None, // never used
    }
}

/// True if `fields` is exactly one well-formed msgpack map (what an LXMF fields element is).
pub fn fields_valid(fields: &[u8]) -> bool {
    let Some(&marker) = fields.first() else {
        return false;
    };
    matches!(marker, 0x80..=0x8f | 0xde | 0xdf) && skip(fields, 0, 0) == Some(fields.len())
}

/// Non-negative msgpack integer at `pos` (any width), with the position past it.
fn read_uint(data: &[u8], pos: usize) -> Option<(u64, usize)> {
    let marker = *data.get(pos)?;
    let p = pos + 1;
    let signed = |v: i64, end: usize| (v >= 0).then_some((v as u64, end));
    match marker {
        0x00..=0x7f => Some((marker as u64, p)),
        0xcc => Some((be(data, p, 1)? as u64, p + 1)),
        0xcd => Some((be(data, p, 2)? as u64, p + 2)),
        0xce => Some((be(data, p, 4)? as u64, p + 4)),
        0xcf => Some((u64::from_be_bytes(data.get(p..p + 8)?.try_into().ok()?), p + 8)),
        0xd0 => signed(*data.get(p)? as i8 as i64, p + 1),
        0xd1 => signed(i16::from_be_bytes(data.get(p..p + 2)?.try_into().ok()?) as i64, p + 2),
        0xd2 => signed(i32::from_be_bytes(data.get(p..p + 4)?.try_into().ok()?) as i64, p + 4),
        0xd3 => signed(i64::from_be_bytes(data.get(p..p + 8)?.try_into().ok()?), p + 8),
        _ => None,
    }
}

// ---- audio field ----------------------------------------------------------------------------

/// Write the msgpack bytes that precede the audio data in a fields map holding only the audio
/// field: `{0x07: [mode, bin(audio_len)]}` minus the `audio_len` data bytes. The complete fields
/// element is this header followed by the audio bytes. Encoded like Python msgpack (`use_bin_type`):
/// fixmap, positive fixint key, fixarray, smallest uint for the mode, smallest bin header.
pub fn audio_field_header(mode: u8, audio_len: usize, out: &mut [u8]) -> Option<usize> {
    let mut header = [0u8; RS_HANDHELD_LXMF_AUDIO_HEADER_MAX];
    header[..3].copy_from_slice(&[0x81, RS_HANDHELD_LXMF_FIELD_AUDIO, 0x92]);
    let mut n = 3;
    if mode < 0x80 {
        header[n] = mode;
        n += 1;
    } else {
        header[n] = 0xcc;
        header[n + 1] = mode;
        n += 2;
    }
    n += write_bin_header(audio_len, &mut header[n..])?;
    out.get_mut(..n)?.copy_from_slice(&header[..n]);
    Some(n)
}

/// The audio field of a raw fields map: `(audio_mode, audio_bytes)` (a sub-slice of `fields`).
/// `None` if there is no `FIELD_AUDIO`, or its value is not `[uint <= 255, bin|str, ...]`.
pub fn find_audio(fields: &[u8]) -> Option<(u8, &[u8])> {
    let value = lxmf_codec::field_value(fields, RS_HANDHELD_LXMF_FIELD_AUDIO)?;
    let (count, pos) = match *value.first()? {
        m @ 0x90..=0x9f => ((m & 0x0f) as usize, 1),
        0xdc => (be(value, 1, 2)?, 3),
        0xdd => (be(value, 1, 4)?, 5),
        _ => return None,
    };
    if count < 2 {
        return None;
    }
    let (mode, after_mode) = read_uint(value, pos)?;
    let mode = u8::try_from(mode).ok()?;
    let rest = value.get(after_mode..)?;
    let audio = lxmf_codec::value_as_bytes(rest)?;
    Some((mode, audio))
}

// ---- building messages with fields ----------------------------------------------------------

/// Exact msgpack payload length of `[timestamp, title, content, fields]` (`fields_len == 0`
/// means the empty map). `None` if a length cannot be represented.
pub fn payload_len(title_len: usize, content_len: usize, fields_len: usize) -> Option<usize> {
    if u32::try_from(title_len).is_err() || u32::try_from(content_len).is_err() {
        return None;
    }
    2usize
        .checked_add(8)?
        .checked_add(bin_header_len(title_len))?
        .checked_add(title_len)?
        .checked_add(bin_header_len(content_len))?
        .checked_add(content_len)?
        .checked_add(fields_len.max(1))
}

/// Pack `[timestamp, title, content, fields]` into `out` (empty `fields` = `{}`).
fn pack_payload(
    timestamp: f64,
    title: &[u8],
    content: &[u8],
    fields: &[u8],
    out: &mut [u8],
) -> Result<usize, LxmfError> {
    let fields = if fields.is_empty() { EMPTY_FIELDS } else { fields };
    let len = payload_len(title.len(), content.len(), fields.len()).ok_or(LxmfError::TooLong)?;
    if len > out.len() {
        return Err(LxmfError::TooLong);
    }
    out[0] = 0x94;
    out[1] = 0xcb;
    out[2..10].copy_from_slice(&timestamp.to_be_bytes());
    let mut at = 10;
    for part in [title, content] {
        at += write_bin_header(part.len(), &mut out[at..]).ok_or(LxmfError::TooLong)?;
        out[at..at + part.len()].copy_from_slice(part);
        at += part.len();
    }
    out[at..at + fields.len()].copy_from_slice(fields);
    Ok(at + fields.len())
}

/// The recipient's `lxmf.delivery` destination hash.
fn delivery_hash(recipient_public_key: &[u8; 64]) -> [u8; 16] {
    destination_hash_from_parts(
        &name_hash(LXMF_DELIVERY_NAME),
        Some(&identity_hash(recipient_public_key)),
    )
}

/// Build the FULL packed LXMF message `dest || source || signature || payload` into `out`, using
/// `scratch` (>= 64 + payload bytes) for the hashed/signed region. Returns the packed length,
/// the recipient dest hash and the message id (= sha256(dest || source || payload)).
#[allow(clippy::too_many_arguments)]
pub fn build_packed(
    source: &LocalIdentity,
    recipient_public_key: &[u8; 64],
    timestamp: f64,
    title: &[u8],
    content: &[u8],
    fields: &[u8],
    out: &mut [u8],
    scratch: &mut [u8],
) -> Result<(usize, [u8; 16], [u8; 32]), LxmfError> {
    if !fields.is_empty() && !fields_valid(fields) {
        return Err(LxmfError::MalformedPayload);
    }
    if scratch.len() < 64 + 11 {
        return Err(LxmfError::OutputTooSmall);
    }
    let dest_hash = delivery_hash(recipient_public_key);
    let source_hash = source.lxmf_delivery_hash();
    // scratch = dest(16) || source(16) || payload(plen) || hash(32); payload packed in place.
    let scratch_len = scratch.len();
    let plen = pack_payload(timestamp, title, content, fields, &mut scratch[32..scratch_len - 32])?;
    if out.len() < PACKED_PREFIX + plen {
        return Err(LxmfError::TooLong);
    }
    scratch[..16].copy_from_slice(&dest_hash);
    scratch[16..32].copy_from_slice(&source_hash);
    let hash = sha256(&scratch[..32 + plen]);
    scratch[32 + plen..64 + plen].copy_from_slice(&hash);
    let signature = source.sign(&scratch[..64 + plen]);
    out[..16].copy_from_slice(&dest_hash);
    out[16..32].copy_from_slice(&source_hash);
    out[32..96].copy_from_slice(&signature);
    out[PACKED_PREFIX..PACKED_PREFIX + plen].copy_from_slice(&scratch[32..32 + plen]);
    Ok((PACKED_PREFIX + plen, dest_hash, hash))
}

/// Single-frame OPPORTUNISTIC message with fields: the packed message minus its leading dest hash,
/// ECIES-encrypted to the recipient (its remembered `ratchet` when known, else its base key).
#[allow(clippy::too_many_arguments)]
pub fn build_opportunistic(
    source: &LocalIdentity,
    recipient_public_key: &[u8; 64],
    ratchet: Option<&[u8; 32]>,
    timestamp: f64,
    title: &[u8],
    content: &[u8],
    fields: &[u8],
    ephemeral_priv: &[u8; 32],
    iv: &[u8; 16],
    out: &mut [u8],
) -> Result<(usize, [u8; 16], [u8; 32]), LxmfError> {
    const MAX_PLAIN: usize = crypto::MAX_ECIES_PLAINTEXT;
    let plen = payload_len(title.len(), content.len(), fields.len()).ok_or(LxmfError::TooLong)?;
    if PLAINTEXT_PREFIX + plen > MAX_PLAIN {
        return Err(LxmfError::TooLong);
    }
    let mut packed = [0u8; 16 + MAX_PLAIN];
    let mut scratch = [0u8; 64 + MAX_PLAIN];
    let (n, dest, id) = build_packed(
        source,
        recipient_public_key,
        timestamp,
        title,
        content,
        fields,
        &mut packed,
        &mut scratch,
    )?;
    let target_x25519: [u8; 32] = match ratchet {
        Some(r) => *r,
        None => recipient_public_key[..32].try_into().unwrap(),
    };
    let len = crypto::ecies_encrypt(
        &packed[16..n],
        &target_x25519,
        &identity_hash(recipient_public_key),
        ephemeral_priv,
        iv,
        out,
    )?;
    Ok((len, dest, id))
}

// ---- C ABI ----------------------------------------------------------------------------------

/// Borrow `len` bytes at `ptr` (null allowed iff `len == 0`).
///
/// # Safety
/// A non-null `ptr` must have `len` readable bytes that outlive the returned slice.
unsafe fn input<'a>(ptr: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        Some(&[])
    } else if ptr.is_null() {
        None
    } else {
        Some(unsafe { core::slice::from_raw_parts(ptr, len) })
    }
}

fn status_of(error: LxmfError) -> RsHandheldStatus {
    match error {
        LxmfError::TooLong | LxmfError::OutputTooSmall => RsHandheldStatus::ErrCapacity,
        LxmfError::MalformedPayload => RsHandheldStatus::ErrInvalidArg,
        LxmfError::Crypto => RsHandheldStatus::ErrCrypto,
        _ => RsHandheldStatus::ErrInternal,
    }
}

/// [`rs_handheld_lxmf_packed_size`] with a fields element of `fields_len` bytes (0 = empty map).
///
/// # Safety
/// `out_size` is writable and aligned. It is unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_packed_size_fields(
    title_len: usize,
    content_len: usize,
    fields_len: usize,
    out_size: *mut usize,
) -> RsHandheldStatus {
    if out_size.is_null() {
        return RsHandheldStatus::ErrInvalidArg;
    }
    match payload_len(title_len, content_len, fields_len).and_then(|n| n.checked_add(PACKED_PREFIX))
    {
        Some(size) => {
            unsafe { *out_size = size };
            RsHandheldStatus::Ok
        }
        None => RsHandheldStatus::ErrCapacity,
    }
}

/// [`rs_handheld_rns_lxmf_build`] with a raw msgpack fields map (`fields_len == 0` = none; a
/// non-empty `fields` must be exactly one well-formed map, else `ErrInvalidArg`).
///
/// # Safety
/// As [`rs_handheld_rns_lxmf_build`]; `fields` has `fields_len` readable bytes (null iff 0).
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_build_fields(
    ctx: *const RsHandheldRns,
    recipient_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    timestamp: f64,
    wall_secs: u64,
    uptime_ms: u64,
    title: *const u8,
    title_len: usize,
    content: *const u8,
    content_len: usize,
    fields: *const u8,
    fields_len: usize,
    ephemeral_priv: *const [u8; 32],
    iv: *const [u8; 16],
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    out_dest_hash: *mut [u8; DESTINATION_LENGTH],
    out_message_id: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || recipient_public_key.is_null()
            || ephemeral_priv.is_null()
            || iv.is_null()
            || out.is_null()
            || out_len.is_null()
            || out_dest_hash.is_null()
            || out_message_id.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: pointers non-null + sized per the contract.
        let (Some(title), Some(content), Some(fields)) = (unsafe { input(title, title_len) }, unsafe {
            input(content, content_len)
        }, unsafe {
            input(fields, fields_len)
        }) else {
            return RsHandheldStatus::ErrInvalidArg;
        };
        let ctx = unsafe { &*ctx };
        let Some(id) = ctx.identity.as_ref() else {
            return RsHandheldStatus::ErrNotReady;
        };
        let recipient = unsafe { &*recipient_public_key };
        let ratchet = ctx.peer_ratchets.get(
            &delivery_hash(recipient),
            RatchetClock::from_host(wall_secs, uptime_ms / 1000),
        );
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        match build_opportunistic(
            id,
            recipient,
            ratchet.as_ref(),
            timestamp,
            title,
            content,
            fields,
            unsafe { &*ephemeral_priv },
            unsafe { &*iv },
            out_slice,
        ) {
            Ok((n, dest, mid)) => {
                unsafe {
                    *out_len = n;
                    *out_dest_hash = dest;
                    *out_message_id = mid;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => status_of(error),
        }
    })
}

/// [`rs_handheld_rns_lxmf_build_link`] with a raw msgpack fields map (see
/// [`rs_handheld_rns_lxmf_build_fields`]).
///
/// # Safety
/// As [`rs_handheld_rns_lxmf_build_link`]; `fields` has `fields_len` readable bytes (null iff 0).
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_build_link_fields(
    ctx: *const RsHandheldRns,
    recipient_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    timestamp: f64,
    title: *const u8,
    title_len: usize,
    content: *const u8,
    content_len: usize,
    fields: *const u8,
    fields_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    out_dest_hash: *mut [u8; DESTINATION_LENGTH],
    out_message_id: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || recipient_public_key.is_null()
            || out.is_null()
            || out_len.is_null()
            || out_dest_hash.is_null()
            || out_message_id.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let (Some(title), Some(content), Some(fields)) = (unsafe { input(title, title_len) }, unsafe {
            input(content, content_len)
        }, unsafe {
            input(fields, fields_len)
        }) else {
            return RsHandheldStatus::ErrInvalidArg;
        };
        let Some(id) = unsafe { &*ctx }.identity.as_ref() else {
            return RsHandheldStatus::ErrNotReady;
        };
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        // Scratch off the task stack, as in rs_handheld_rns_lxmf_build_link.
        let mut scratch: Box<[u8; LXMF_PACKED_MAX]> = match box_zeroed() {
            Some(b) => b,
            None => return RsHandheldStatus::ErrInternal,
        };
        match build_packed(
            id,
            unsafe { &*recipient_public_key },
            timestamp,
            title,
            content,
            fields,
            out_slice,
            scratch.as_mut_slice(),
        ) {
            Ok((n, dest, mid)) => {
                unsafe {
                    *out_len = n;
                    *out_dest_hash = dest;
                    *out_message_id = mid;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => status_of(error),
        }
    })
}

/// Write the fields-map bytes that precede `audio_len` audio bytes (see [`audio_field_header`]);
/// the full fields element = these `*out_len` bytes followed by the audio data.
/// `ErrCapacity` if `out_cap` < the header (use [`RS_HANDHELD_LXMF_AUDIO_HEADER_MAX`]).
///
/// # Safety
/// `out` has `out_cap` writable bytes; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_audio_field_header(
    mode: u8,
    audio_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    if out.is_null() || out_len.is_null() {
        return RsHandheldStatus::ErrInvalidArg;
    }
    let out = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
    match audio_field_header(mode, audio_len, out) {
        Some(n) => {
            unsafe { *out_len = n };
            RsHandheldStatus::Ok
        }
        None => RsHandheldStatus::ErrCapacity,
    }
}

/// Find `FIELD_AUDIO` in a raw fields map: `*out_mode` = audio mode, the audio bytes are
/// `fields[*out_offset .. *out_offset + *out_len]`. `ErrUnsupported` if there is no well-formed
/// audio field (absent, or not `[uint <= 255, bin]`); outputs are then unchanged.
///
/// # Safety
/// `fields` has `fields_len` readable bytes (null iff 0); outputs writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_fields_audio(
    fields: *const u8,
    fields_len: usize,
    out_mode: *mut u8,
    out_offset: *mut usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    if out_mode.is_null() || out_offset.is_null() || out_len.is_null() {
        return RsHandheldStatus::ErrInvalidArg;
    }
    let Some(fields) = (unsafe { input(fields, fields_len) }) else {
        return RsHandheldStatus::ErrInvalidArg;
    };
    match find_audio(fields) {
        Some((mode, audio)) => {
            unsafe {
                *out_mode = mode;
                *out_offset = audio.as_ptr() as usize - fields.as_ptr() as usize;
                *out_len = audio.len();
            }
            RsHandheldStatus::Ok
        }
        None => RsHandheldStatus::ErrUnsupported,
    }
}

#[cfg(test)]
#[path = "fields_tests.rs"]
mod fields_tests;
