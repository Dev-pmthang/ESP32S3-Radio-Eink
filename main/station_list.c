/*
 * station_list.c - English-language stations, MP3 only.
 *
 * Every URL below was probed on 2026-11-14 with
 *   curl -v -L -H "Icy-MetaData: 1" <url>
 * and answered HTTP 200 with Content-Type: audio/mpeg. Only MP3 streams are
 * listed because the pipeline uses libhelix-mp3 (AAC/HLS stations such as
 * Radio Paradise mellow/rock, Triple J, iHeart feeds are left out).
 *
 * Plain HTTP stations come first: they need no TLS session (~25 KB heap) on
 * the 320 KB ESP32-C5. HTTPS stations follow.
 */
#include "station_list.h"

static const station_t s_stations[] = {
    /* ---- HTTP (no TLS) ------------------------------------------------ */
    {
        .name  = "Radio Paradise - Main Mix",
        .genre = "Eclectic (US)",
        .url   = "http://stream-uk1.radioparadise.com/mp3-128",
        .note  = "HTTP, MP3 128 kbps, icy-metaint 16000",
    },
    {
        .name  = "KEXP 90.3 FM Seattle",
        .genre = "Indie / Alternative (US)",
        .url   = "http://kexp-mp3-128.streamguys1.com/kexp128.mp3",
        .note  = "HTTP, MP3 128 kbps, live DJs",
    },
    {
        .name  = "181.FM The Eagle",
        .genre = "Classic Rock (US)",
        .url   = "http://listen.181fm.com/181-eagle_128k.mp3",
        .note  = "HTTP, MP3 128 kbps",
    },
    {
        .name  = "WFMU Freeform Radio",
        .genre = "Freeform (US)",
        .url   = "http://stream0.wfmu.org/freeform-128k",
        .note  = "HTTP, MP3 128 kbps, listener-supported New Jersey station",
    },
    {
        .name  = "BBC World Service",
        .genre = "News / Talk (UK)",
        .url   = "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service",
        .note  = "HTTP, MP3 56 kbps (low bitrate speech feed)",
    },
    {
        .name  = "SomaFM Groove Salad",
        .genre = "Ambient Chill (US)",
        .url   = "http://ice1.somafm.com/groovesalad-128-mp3",
        .note  = "HTTP/1.0 Icecast, MP3 128 kbps, icy-metaint 45000",
    },
    {
        .name  = "SomaFM Secret Agent",
        .genre = "Lounge / Spy Jazz (US)",
        .url   = "http://ice1.somafm.com/secretagent-128-mp3",
        .note  = "HTTP/1.0 Icecast, MP3 128 kbps",
    },
    {
        .name  = "SomaFM Indie Pop Rocks",
        .genre = "Indie Pop (US)",
        .url   = "http://ice1.somafm.com/indiepop-128-mp3",
        .note  = "HTTP/1.0 Icecast, MP3 128 kbps",
    },
    {
        .name  = "SomaFM Beat Blender",
        .genre = "Deep House / Downtempo (US)",
        .url   = "http://ice1.somafm.com/beatblender-128-mp3",
        .note  = "HTTP/1.0 Icecast, MP3 128 kbps",
    },
    {
        .name  = "SomaFM Lush",
        .genre = "Vocal Electronica (US)",
        .url   = "http://ice1.somafm.com/lush-128-mp3",
        .note  = "HTTP/1.0 Icecast, MP3 128 kbps",
    },
    {
        .name  = "SomaFM Drone Zone",
        .genre = "Ambient Space (US)",
        .url   = "http://ice1.somafm.com/dronezone-128-mp3",
        .note  = "HTTP/1.0 Icecast, MP3 128 kbps",
    },
    {
        .name  = "SomaFM Deep Space One",
        .genre = "Deep Ambient (US)",
        .url   = "http://ice1.somafm.com/deepspaceone-128-mp3",
        .note  = "HTTP/1.0 Icecast, MP3 128 kbps",
    },
    {
        .name  = "Public Domain Jazz & Swing",
        .genre = "Jazz / Swing (instrumental)",
        .url   = "http://relay.publicdomainradio.org/jazz_swing.mp3",
        .note  = "HTTP, MP3, no announcers",
    },
    {
        .name  = "Dance Wave!",
        .genre = "Dance / EDM",
        .url   = "http://stream.dancewave.online/dance.mp3",
        .note  = "HTTP, MP3, 1 redirect",
    },
    /* ---- HTTPS (TLS, ~25 KB more heap while playing) ---------------- */
    {
        .name  = "NPR Program Stream",
        .genre = "News / Talk (US)",
        .url   = "https://npr-ice.streamguys1.com/live.mp3",
        .note  = "HTTPS, MP3 96 kbps",
    },
    {
        .name  = "WQXR New York",
        .genre = "Classical (US)",
        .url   = "https://stream.wqxr.org/wqxr",
        .note  = "HTTPS, MP3 128 kbps",
    },
    {
        .name  = "Classic FM",
        .genre = "Classical (UK)",
        .url   = "https://media-ice.musicradio.com/ClassicFMMP3",
        .note  = "HTTPS, HTTP/1.0, MP3 128 kbps, icy-metaint 8000",
    },
    {
        .name  = "Lofi Radio",
        .genre = "Chill / Lofi (music only)",
        .url   = "https://stream.laut.fm/lofi",
        .note  = "HTTPS, MP3 128 kbps, 302 redirect (two TLS handshakes)",
    },
    /* ---- local ---------------------------------------------------------- */
    {
        .name  = "Local Music Server (PC LAN)",
        .genre = "Local",
        .url   = "http://192.168.111.178:8000/song.mp3",
        .note  = "server/server.py from the ESP32-S3 project; edit the IP to your PC",
    },
};

size_t station_list_count(void)
{
    return sizeof(s_stations) / sizeof(s_stations[0]);
}

const station_t *station_list_get(size_t index)
{
    if (index >= station_list_count()) {
        return NULL;
    }
    return &s_stations[index];
}
