//! LXMF fields / audio field tests.
//!
//! The Python LXMF library is not installed on the dev machines, so the expected msgpack bytes
//! below are built BY HAND from the msgpack spec and the way Python LXMF packs a message
//! (`msgpack.packb([timestamp, title, content, fields])`, bytes as `bin`, ints as the smallest
//! encoding, lists as fixarray): e.g. `fields = {0x07: [0x09, b"\x01\x02\x03"]}` packs to
//! `81 07 92 09 c4 03 01 02 03`. The byte-exact link with Python is the existing pinned vector
//! (`lxmf_build_byte_exact_with_python`): the fields builder reproduces it exactly when there are
//! no fields, and differs from it only in the fields element otherwise.

use super::*;

fn hex(s: &str) -> Vec<u8> {
    let s: String = s.split_whitespace().collect();
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}

fn ctx_with(prv: &[u8; 64]) -> *mut RsHandheldRns {
    let mut ctx = core::ptr::null_mut();
    unsafe {
        assert_eq!(rs_handheld_rns_init(&mut ctx), RsHandheldStatus::Ok);
        assert_eq!(rs_handheld_rns_load_identity(ctx, prv), RsHandheldStatus::Ok);
    }
    ctx
}

/// `{0x07: [mode, bin(audio)]}` the way Python msgpack packs it.
fn audio_fields(mode: u8, audio: &[u8]) -> Vec<u8> {
    let mut header = [0u8; RS_HANDHELD_LXMF_AUDIO_HEADER_MAX];
    let n = audio_field_header(mode, audio.len(), &mut header).unwrap();
    let mut v = header[..n].to_vec();
    v.extend_from_slice(audio);
    v
}

#[test]
fn lxmf_constants_match_lxmf_py() {
    assert_eq!(RS_HANDHELD_LXMF_FIELD_AUDIO, 0x07);
    assert_eq!(RS_HANDHELD_LXMF_AM_CODEC2_700C, 0x03);
    assert_eq!(RS_HANDHELD_LXMF_AM_CODEC2_1600, 0x07);
    assert_eq!(RS_HANDHELD_LXMF_AM_CODEC2_3200, 0x09);
}

#[test]
fn audio_field_header_encodes_like_python_msgpack() {
    let mut out = [0u8; RS_HANDHELD_LXMF_AUDIO_HEADER_MAX];
    let cases: &[(u8, usize, &str)] = &[
        (0x09, 3, "81 07 92 09 c4 03"),
        (0x03, 0, "81 07 92 03 c4 00"),
        (0x07, 255, "81 07 92 07 c4 ff"),
        (0x07, 256, "81 07 92 07 c5 01 00"),
        (0x7f, 3000, "81 07 92 7f c5 0b b8"),
        (0x80, 1, "81 07 92 cc 80 c4 01"),
        (0xff, 70000, "81 07 92 cc ff c6 00 01 11 70"),
    ];
    for &(mode, len, expected) in cases {
        let n = audio_field_header(mode, len, &mut out).unwrap();
        assert_eq!(&out[..n], hex(expected).as_slice(), "mode {mode} len {len}");
    }
    assert_eq!(audio_field_header(0xff, 70000, &mut [0u8; 9]), None);
    // C ABI wrapper.
    let mut len = 0;
    unsafe {
        assert_eq!(
            rs_handheld_lxmf_audio_field_header(0x09, 3, out.as_mut_ptr(), out.len(), &mut len),
            RsHandheldStatus::Ok
        );
        assert_eq!(&out[..len], hex("81 07 92 09 c4 03").as_slice());
        assert_eq!(
            rs_handheld_lxmf_audio_field_header(0x09, 3, out.as_mut_ptr(), 4, &mut len),
            RsHandheldStatus::ErrCapacity
        );
    }
}

#[test]
fn find_audio_reads_sideband_style_fields() {
    // Only the audio field.
    let only = hex("81 07 92 09 c4 03 01 02 03");
    assert_eq!(find_audio(&only), Some((0x09, &[1u8, 2, 3][..])));
    // Among other fields, Sideband style: {0x04: [b"icon", b"\x00\x00\x00", b"\xff\xff\xff"],
    // 0x07: [0x03, bin16(300 bytes)], 0xfb: "x"} — keys in any order, other values skipped.
    let audio: Vec<u8> = (0..300).map(|i| (i * 7) as u8).collect();
    let mut many = hex("83 04 93 c4 04 69636f6e c4 03 000000 c4 03 ffffff 07 92 03 c5 01 2c");
    many.extend_from_slice(&audio);
    many.extend_from_slice(&hex("cc fb a1 78"));
    assert!(fields_valid(&many));
    assert_eq!(find_audio(&many), Some((0x03, audio.as_slice())));
    // Mode as uint8 (0xcc) / int8 (0xd0), audio as str, extra trailing array element, array16.
    assert_eq!(find_audio(&hex("81 07 92 cc 09 c4 01 aa")), Some((9, &[0xaa][..])));
    assert_eq!(find_audio(&hex("81 07 92 d0 09 c4 01 aa")), Some((9, &[0xaa][..])));
    assert_eq!(find_audio(&hex("81 07 92 09 a2 6869")), Some((9, &b"hi"[..])));
    assert_eq!(find_audio(&hex("81 07 93 09 c4 01 aa c0")), Some((9, &[0xaa][..])));
    assert_eq!(find_audio(&hex("81 07 dc 00 02 09 c4 01 aa")), Some((9, &[0xaa][..])));
    // Not an audio field.
    for bad in [
        "80",                         // empty map
        "81 06 92 09 c4 01 aa",       // other key (FIELD_IMAGE)
        "81 07 c4 01 aa",             // value not an array
        "81 07 91 09",                // one element
        "81 07 92 cd 01 00 c4 01 aa", // mode 256
        "81 07 92 d0 ff c4 01 aa",    // mode -1
        "81 07 92 09 09",             // audio not bin/str
        "81 07 92 09 c4 05 aa",       // truncated audio
        "",
    ] {
        assert_eq!(find_audio(&hex(bad)), None, "{bad}");
    }
    // C ABI: offset/length into the caller's buffer.
    let (mut mode, mut offset, mut len) = (0u8, 0usize, 0usize);
    unsafe {
        assert_eq!(
            rs_handheld_lxmf_fields_audio(many.as_ptr(), many.len(), &mut mode, &mut offset, &mut len),
            RsHandheldStatus::Ok
        );
        assert_eq!((mode, &many[offset..offset + len]), (3, audio.as_slice()));
        assert_eq!(
            rs_handheld_lxmf_fields_audio([0x80].as_ptr(), 1, &mut mode, &mut offset, &mut len),
            RsHandheldStatus::ErrUnsupported
        );
        assert_eq!(
            rs_handheld_lxmf_fields_audio(core::ptr::null(), 0, &mut mode, &mut offset, &mut len),
            RsHandheldStatus::ErrUnsupported
        );
    }
}

#[test]
fn fields_valid_accepts_exactly_one_map() {
    for good in ["80", "81 07 92 09 c4 03 010203", "de 00 01 01 c0", "82 01 cb 3ff0000000000000 02 d6 ff 00000000"] {
        assert!(fields_valid(&hex(good)), "{good}");
    }
    for bad in [
        "",
        "92 01 02",                // array, not a map
        "81 07",                   // missing value
        "81 07 c4 05 01",          // truncated bin
        "80 80",                   // trailing bytes
        "81 07 c6 ff ff ff ff 00", // bin32 length past the end (no wrap)
        "df ff ff ff ff",          // huge map count, no entries
        "81 07 c1",                // reserved marker
    ] {
        assert!(!fields_valid(&hex(bad)), "{bad}");
    }
    // Nesting depth is bounded.
    let mut deep = vec![0x81, 0x01];
    deep.extend(core::iter::repeat_n(0x91, 20));
    deep.push(0xc0);
    assert!(!fields_valid(&deep));
}

#[test]
fn packed_message_with_audio_matches_hand_built_lxmf_bytes() {
    let source = LocalIdentity::from_private_key(&[0x11; 64]);
    let recipient = LocalIdentity::from_private_key(&[0x22; 64]);
    let fields = hex("81 07 92 09 c4 03 01 02 03");
    let mut out = [0u8; 512];
    let mut scratch = [0u8; 512];
    let (n, dest, id) = build_packed(
        &source,
        recipient.public_key(),
        1234567890.5,
        b"",
        b"voice",
        &fields,
        &mut out,
        &mut scratch,
    )
    .unwrap();
    // [1234567890.5, b"", b"voice", {7: [9, b"\x01\x02\x03"]}]
    let payload = hex("94 cb 41d26580b4a00000 c4 00 c4 05 766f696365 81 07 92 09 c4 03 010203");
    assert_eq!(&out[96..n], payload.as_slice());
    assert_eq!(dest, recipient.lxmf_delivery_hash());
    assert_eq!(&out[..16], &dest);
    assert_eq!(&out[16..32], &source.lxmf_delivery_hash());
    let mut hashed = out[..32].to_vec();
    hashed.extend_from_slice(&payload);
    assert_eq!(id, rns_lite_core::wire::sha256(&hashed));
    assert_eq!(
        payload_len(0, 5, fields.len()).unwrap() + 96,
        n,
        "size function agrees with the builder"
    );
    // The independent lite codec validates it (signature + hash) and returns the same fields.
    let mut scratch2 = [0u8; 512];
    let view = lxmf_codec::parse_link(&recipient, &out[..n], source.public_key(), &mut scratch2).unwrap();
    assert_eq!(view.message_id, id);
    assert_eq!(view.content, b"voice");
    assert_eq!(view.fields, fields.as_slice());
    assert_eq!(find_audio(view.fields), Some((9, &[1u8, 2, 3][..])));
}

#[test]
fn no_fields_is_byte_identical_to_the_lite_codec() {
    let source = LocalIdentity::from_private_key(&[0x11; 64]);
    let recipient = LocalIdentity::from_private_key(&[0x22; 64]);
    for (title, content) in [(&b""[..], &b""[..]), (b"Hi", b"hello world"), (b"t", &[0x5a; 3000][..])] {
        let mut ours = vec![0u8; 4096];
        let mut theirs = vec![0u8; 4096];
        let mut scratch = vec![0u8; 4096];
        let (n, dest, id) = build_packed(
            &source,
            recipient.public_key(),
            42.25,
            title,
            content,
            &[],
            &mut ours,
            &mut scratch,
        )
        .unwrap();
        let (mut d2, mut id2) = ([0u8; 16], [0u8; 32]);
        let m = lxmf_codec::build_link(
            &source,
            recipient.public_key(),
            42.25,
            title,
            content,
            &mut theirs,
            &mut scratch,
            &mut d2,
            &mut id2,
        )
        .unwrap();
        assert_eq!(&ours[..n], &theirs[..m]);
        assert_eq!((dest, id), (d2, id2));
        // An explicit empty map is the same message.
        let (n3, _, id3) = build_packed(
            &source,
            recipient.public_key(),
            42.25,
            title,
            content,
            &[0x80],
            &mut ours,
            &mut scratch,
        )
        .unwrap();
        assert_eq!((n3, id3), (m, id2));
        let mut size = 0;
        let mut size_plain = 0;
        unsafe {
            assert_eq!(
                rs_handheld_lxmf_packed_size_fields(title.len(), content.len(), 0, &mut size),
                RsHandheldStatus::Ok
            );
            assert_eq!(
                rs_handheld_lxmf_packed_size(title.len(), content.len(), &mut size_plain),
                RsHandheldStatus::Ok
            );
        }
        assert_eq!((size, size_plain), (m, m));
    }
}

#[test]
fn opportunistic_fields_build_reproduces_the_python_vector_without_fields() {
    // Same inputs as tests::lxmf_build_byte_exact_with_python (pinned vs Python LXMF).
    let incrementing: [u8; 64] = core::array::from_fn(|i| i as u8);
    let recipient_prv: [u8; 64] = core::array::from_fn(|i| 0x40 + i as u8);
    let recipient = LocalIdentity::from_private_key(&recipient_prv);
    let ctx = ctx_with(&incrementing);
    let mut out = [0u8; 600];
    let mut out_len = 0;
    let mut dest = [0u8; 16];
    let mut mid = [0u8; 32];
    let mut plain = [0u8; 600];
    let mut plain_len = 0;
    let (mut d2, mut m2) = ([0u8; 16], [0u8; 32]);
    unsafe {
        assert_eq!(
            rs_handheld_rns_lxmf_build_fields(
                ctx,
                recipient.public_key(),
                1234567890.5,
                1_234_567_890,
                0,
                b"Hi".as_ptr(),
                2,
                b"hello world".as_ptr(),
                11,
                core::ptr::null(),
                0,
                &[0x33; 32],
                &[0x44; 16],
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
                &mut dest,
                &mut mid,
            ),
            RsHandheldStatus::Ok
        );
        assert_eq!(
            rs_handheld_rns_lxmf_build(
                ctx,
                recipient.public_key(),
                1234567890.5,
                1_234_567_890,
                0,
                b"Hi".as_ptr(),
                2,
                b"hello world".as_ptr(),
                11,
                &[0x33; 32],
                &[0x44; 16],
                plain.as_mut_ptr(),
                plain.len(),
                &mut plain_len,
                &mut d2,
                &mut m2,
            ),
            RsHandheldStatus::Ok
        );
        rs_handheld_rns_shutdown(ctx);
    }
    assert_eq!(&out[..out_len], &plain[..plain_len]);
    assert_eq!((dest, mid), (d2, m2));
    assert_eq!(
        mid.iter().map(|b| format!("{b:02x}")).collect::<String>(),
        "dceab683f560aad56d128fc7051ceb41ef4cd935430df0eb089a4ea0095be1a4"
    );
}

#[test]
fn opportunistic_audio_message_round_trips_through_parse() {
    let tx = ctx_with(&[0x11; 64]);
    let rx = ctx_with(&[0x22; 64]);
    let source = LocalIdentity::from_private_key(&[0x11; 64]);
    let recipient = LocalIdentity::from_private_key(&[0x22; 64]);
    // 2 s of Codec2 700C = 25 frames x 7 bytes... (any bytes: the codec is opaque here).
    let audio: Vec<u8> = (0..175).map(|i| (i * 13 + 1) as u8).collect();
    let fields = audio_fields(RS_HANDHELD_LXMF_AM_CODEC2_700C, &audio);
    let mut out = [0u8; 600];
    let (mut out_len, mut dest, mut mid) = (0, [0u8; 16], [0u8; 32]);
    let build = |fields: &[u8], out: &mut [u8; 600], out_len: &mut usize, dest: &mut [u8; 16], mid: &mut [u8; 32]| unsafe {
        rs_handheld_rns_lxmf_build_fields(
            tx,
            recipient.public_key(),
            1700000000.0,
            0,
            0,
            core::ptr::null(),
            0,
            b"hi".as_ptr(),
            2,
            fields.as_ptr(),
            fields.len(),
            &[0x55; 32],
            &[0x66; 16],
            out.as_mut_ptr(),
            out.len(),
            out_len,
            dest,
            mid,
        )
    };
    assert_eq!(build(&fields, &mut out, &mut out_len, &mut dest, &mut mid), RsHandheldStatus::Ok);
    let mut msg: RsHandheldLxmfMessage = unsafe { core::mem::zeroed() };
    unsafe {
        assert_eq!(
            rs_handheld_rns_lxmf_parse(rx, out.as_ptr(), out_len, source.public_key(), &mut msg),
            RsHandheldStatus::Ok
        );
    }
    assert_eq!(msg.message_id, mid);
    assert_eq!(&msg.content[..msg.content_len as usize], b"hi");
    let got = &msg.fields[..msg.fields_len as usize];
    assert_eq!(got, fields.as_slice());
    assert_eq!(find_audio(got), Some((3, audio.as_slice())));
    // A text-only message carries the empty map.
    assert_eq!(build(&[], &mut out, &mut out_len, &mut dest, &mut mid), RsHandheldStatus::Ok);
    unsafe {
        assert_eq!(
            rs_handheld_rns_lxmf_parse(rx, out.as_ptr(), out_len, source.public_key(), &mut msg),
            RsHandheldStatus::Ok
        );
    }
    assert_eq!(&msg.fields[..msg.fields_len as usize], &[0x80]);
    // Too big for one frame -> capacity (the engine then uses a link); malformed -> invalid.
    let big = audio_fields(3, &[0u8; 400]);
    assert_eq!(build(&big, &mut out, &mut out_len, &mut dest, &mut mid), RsHandheldStatus::ErrCapacity);
    assert_eq!(build(&[0x81, 0x07], &mut out, &mut out_len, &mut dest, &mut mid), RsHandheldStatus::ErrInvalidArg);
    unsafe {
        rs_handheld_rns_shutdown(tx);
        rs_handheld_rns_shutdown(rx);
    }
}

#[test]
fn link_audio_message_round_trips_through_parse_link_view() {
    let tx = ctx_with(&[0x11; 64]);
    let rx = ctx_with(&[0x22; 64]);
    let source = LocalIdentity::from_private_key(&[0x11; 64]);
    let recipient = LocalIdentity::from_private_key(&[0x22; 64]);
    for audio_len in [0usize, 1, 255, 256, 3000, 3542] {
        let audio: Vec<u8> = (0..audio_len).map(|i| (i % 251) as u8).collect();
        let fields = audio_fields(RS_HANDHELD_LXMF_AM_CODEC2_3200, &audio);
        let mut packed = vec![0u8; LXMF_PACKED_MAX];
        let (mut len, mut dest, mut mid) = (0, [0u8; 16], [0u8; 32]);
        let mut expected = 0;
        unsafe {
            assert_eq!(
                rs_handheld_lxmf_packed_size_fields(0, 0, fields.len(), &mut expected),
                RsHandheldStatus::Ok
            );
            assert_eq!(
                rs_handheld_rns_lxmf_build_link_fields(
                    tx,
                    recipient.public_key(),
                    1700000000.0,
                    core::ptr::null(),
                    0,
                    core::ptr::null(),
                    0,
                    fields.as_ptr(),
                    fields.len(),
                    packed.as_mut_ptr(),
                    packed.len(),
                    &mut len,
                    &mut dest,
                    &mut mid,
                ),
                RsHandheldStatus::Ok,
                "audio {audio_len}"
            );
        }
        assert_eq!(len, expected);
        assert!(len <= LXMF_PACKED_MAX);
        packed.truncate(len);
        let mut view = RsHandheldLxmfView::default();
        unsafe {
            assert_eq!(
                rs_handheld_rns_lxmf_parse_link_view(rx, packed.as_ptr(), len, source.public_key(), &mut view),
                RsHandheldStatus::Ok
            );
        }
        assert_eq!(view.message_id, mid);
        let span = &packed[view.fields_offset as usize..(view.fields_offset + view.fields_len) as usize];
        assert_eq!(span, fields.as_slice());
        let (mut mode, mut offset, mut alen) = (0u8, 0usize, 0usize);
        unsafe {
            assert_eq!(
                rs_handheld_lxmf_fields_audio(span.as_ptr(), span.len(), &mut mode, &mut offset, &mut alen),
                RsHandheldStatus::Ok
            );
        }
        assert_eq!((mode, &span[offset..offset + alen]), (9, audio.as_slice()));
    }
    // One byte over the resource payload limit is refused.
    let mut size = 0;
    unsafe {
        assert_eq!(rs_handheld_lxmf_packed_size_fields(0, 0, 3543 + 7, &mut size), RsHandheldStatus::Ok);
    }
    assert!(size > LXMF_PACKED_MAX);
    let fields = audio_fields(9, &vec![0u8; 3543]);
    let mut packed = vec![0u8; LXMF_PACKED_MAX];
    let (mut len, mut dest, mut mid) = (0, [0u8; 16], [0u8; 32]);
    unsafe {
        assert_eq!(
            rs_handheld_rns_lxmf_build_link_fields(
                tx,
                recipient.public_key(),
                1.0,
                core::ptr::null(),
                0,
                core::ptr::null(),
                0,
                fields.as_ptr(),
                fields.len(),
                packed.as_mut_ptr(),
                packed.len(),
                &mut len,
                &mut dest,
                &mut mid,
            ),
            RsHandheldStatus::ErrCapacity
        );
        rs_handheld_rns_shutdown(tx);
        rs_handheld_rns_shutdown(rx);
    }
}
