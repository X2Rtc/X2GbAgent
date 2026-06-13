PRAGMA journal_mode=DELETE;
PRAGMA synchronous=FULL;
PRAGMA foreign_keys=ON;
PRAGMA user_version=1;

CREATE TABLE IF NOT EXISTS video_config(
  id INTEGER PRIMARY KEY CHECK(id=1),
  codec TEXT NOT NULL,
  resolution TEXT NOT NULL,
  fps INTEGER NOT NULL,
  rc_mode TEXT NOT NULL CHECK(rc_mode IN ('CBR','VBR')),
  bitrate_kbps INTEGER NOT NULL,
  gop INTEGER NOT NULL,
  iframe_interval INTEGER NOT NULL,
  low_latency INTEGER NOT NULL DEFAULT 1,
  prefer_hardware INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS audio_config(
  id INTEGER PRIMARY KEY CHECK(id=1),
  enabled INTEGER NOT NULL,
  codec TEXT NOT NULL,
  sample_rate INTEGER NOT NULL,
  bitrate_kbps INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS device_source_config(
  id INTEGER PRIMARY KEY CHECK(id=1),
  source_mode TEXT NOT NULL CHECK(source_mode IN ('device','file','screen')),
  video_device TEXT NOT NULL,
  audio_device TEXT NOT NULL,
  media_file TEXT NOT NULL,
  resolution TEXT NOT NULL,
  bitrate_kbps INTEGER NOT NULL,
  file_loop INTEGER NOT NULL,
  file_pacing TEXT NOT NULL CHECK(file_pacing IN ('realtime','fast')),
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS channels(
  id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND 8),
  enabled INTEGER NOT NULL DEFAULT 0,
  name TEXT NOT NULL DEFAULT '',
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS channel_platform_config(
  channel_id INTEGER PRIMARY KEY,
  server_ip TEXT NOT NULL DEFAULT '',
  sip_port INTEGER NOT NULL DEFAULT 0,
  sip_id TEXT NOT NULL DEFAULT '',
  device_id TEXT NOT NULL DEFAULT '',
  username TEXT NOT NULL DEFAULT '',
  password TEXT NOT NULL DEFAULT '',
  transport TEXT NOT NULL DEFAULT 'UDP' CHECK(transport IN ('UDP','TCP','TrUdp')),
  register_interval INTEGER NOT NULL DEFAULT 0,
  heartbeat_interval INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL,
  FOREIGN KEY(channel_id) REFERENCES channels(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS channel_source_config(
  channel_id INTEGER PRIMARY KEY,
  source_profile TEXT NOT NULL DEFAULT 'none' CHECK(source_profile IN ('none','global','custom')),
  source_mode TEXT NOT NULL DEFAULT 'device' CHECK(source_mode IN ('device','file','screen')),
  video_device TEXT NOT NULL DEFAULT '',
  audio_device TEXT NOT NULL DEFAULT '',
  media_file TEXT NOT NULL DEFAULT '',
  resolution TEXT NOT NULL DEFAULT '',
  bitrate_kbps INTEGER NOT NULL DEFAULT 0,
  file_loop INTEGER NOT NULL DEFAULT 1,
  file_pacing TEXT NOT NULL DEFAULT 'realtime' CHECK(file_pacing IN ('realtime','fast')),
  updated_at INTEGER NOT NULL,
  FOREIGN KEY(channel_id) REFERENCES channels(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS channel_av_config(
  channel_id INTEGER PRIMARY KEY,
  video_codec TEXT NOT NULL DEFAULT 'H264',
  resolution TEXT NOT NULL DEFAULT '1920x1080',
  fps INTEGER NOT NULL DEFAULT 25,
  rc_mode TEXT NOT NULL DEFAULT 'CBR' CHECK(rc_mode IN ('CBR','VBR')),
  video_bitrate_kbps INTEGER NOT NULL DEFAULT 4096,
  gop INTEGER NOT NULL DEFAULT 50,
  iframe_interval INTEGER NOT NULL DEFAULT 2,
  low_latency INTEGER NOT NULL DEFAULT 1,
  prefer_hardware INTEGER NOT NULL DEFAULT 0,
  audio_enabled INTEGER NOT NULL DEFAULT 1,
  audio_codec TEXT NOT NULL DEFAULT 'G711A',
  sample_rate INTEGER NOT NULL DEFAULT 8000,
  audio_bitrate_kbps INTEGER NOT NULL DEFAULT 64,
  updated_at INTEGER NOT NULL,
  FOREIGN KEY(channel_id) REFERENCES channels(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS logs(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  level TEXT NOT NULL,
  category TEXT NOT NULL,
  message TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS ota_jobs(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  filename TEXT NOT NULL,
  size_bytes INTEGER NOT NULL,
  status TEXT NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS console_auth(
  id INTEGER PRIMARY KEY CHECK(id=1),
  username TEXT NOT NULL,
  password TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_logs_ts ON logs(ts);
CREATE INDEX IF NOT EXISTS idx_ota_jobs_created_at ON ota_jobs(created_at);

INSERT OR REPLACE INTO video_config(
  id, codec, resolution, fps, rc_mode, bitrate_kbps, gop,
  iframe_interval, low_latency, prefer_hardware, updated_at
) VALUES (
  1, 'H264', '1920x1080', 25, 'CBR', 4096, 50,
  2, 1, 0, 0
);

INSERT OR REPLACE INTO audio_config(
  id, enabled, codec, sample_rate, bitrate_kbps, updated_at
) VALUES (
  1, 1, 'G711A', 8000, 64, 0
);

INSERT OR REPLACE INTO device_source_config(
  id, source_mode, video_device, audio_device, media_file, resolution,
  bitrate_kbps, file_loop, file_pacing, updated_at
) VALUES (
  1, 'device', '', '', 'data/Big_Buck_Bunny_720_10s_2MB.mp4', '1280x720',
  1600, 1, 'realtime', 0
);

INSERT OR REPLACE INTO console_auth(
  id, username, password, updated_at
) VALUES (
  1, 'admin', 'anyrtc', 0
);
