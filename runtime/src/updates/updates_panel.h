// What the UPDATES card and its panel say, and what their buttons do. Pure logic, header only, no platform headers, no
// exceptions: PC and Quest both use it. Tests: tests/updates/test_updates_panel.cpp.
//
// The loader checks the feed and keeps the verified offer; the runtime reads its status (state, version, changelog,
// size, progress) and asks it to start, skip or relaunch. The panel is the stock UPDATE NEWS pop-up: a header, the
// news lines and two buttons. Left (stock LEARN MORE) is SKIP or LATER and closes the panel; right (stock CONTINUE) is
// UPDATE, then RESTART NOW.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace evr_updates_panel {

enum class State { kChecking, kNone, kOffer, kDownloading, kStaged, kFailed };

struct Status {
  State state = State::kChecking;
  std::string version, title, error;
  std::vector<std::string> notes;
  std::uint64_t files = 0, bytes = 0, done = 0, total = 0;
  bool relaunch = false;  // staged files apply at the next launch
  bool skipped = false;   // the player skipped this version before
};

inline State StateFromName(const std::string& s) {
  if (s == "none") return State::kNone;
  if (s == "offer") return State::kOffer;
  if (s == "downloading") return State::kDownloading;
  if (s == "staged") return State::kStaged;
  if (s == "failed") return State::kFailed;
  return State::kChecking;
}

// Texts the game's font can draw: printable ASCII, upper case (the menu's style), at most `limit` bytes.
inline std::string Clean(const std::string& s, std::size_t limit) {
  std::string out;
  for (char c : s) {
    if (out.size() >= limit) break;
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) c = u == '\n' || u == '\t' ? ' ' : '?';
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    out.push_back(c);
  }
  return out;
}

inline std::string Megabytes(std::uint64_t bytes) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return b;
}

/// The card (and so its panel) exists only while there is an update to act on.
inline bool CardVisible(const Status& s) {
  return s.state == State::kOffer || s.state == State::kDownloading || s.state == State::kStaged ||
         s.state == State::kFailed;
}

struct Texts {
  std::string card;                 // the card's caption
  std::string header, body, body2;  // the pop-up's header and news lines
  std::string news;                 // body and body2 in the pop-up's one wrapped text area
  std::string left, right;          // its buttons
};

inline constexpr std::size_t kCardChars = 32, kHeaderChars = 40, kLineChars = 90, kNewsChars = 127,
                                 kButtonChars = 24;

inline Texts Compose(const Status& s) {
  Texts t;
  const std::string v = s.version;
  std::string notes;
  for (const std::string& n : s.notes) notes += (notes.empty() ? "" : " / ") + n;
  t.card = "UPDATE";
  switch (s.state) {
    case State::kChecking:
    case State::kNone:
      t.header = "ECHO VR IS UP TO DATE";
      t.left = "CLOSE";
      t.right = "CLOSE";
      break;
    case State::kOffer:
      t.header = s.title.empty() ? "UPDATE " + v : s.title;
      t.body = notes;
      t.body2 = "VERSION " + v + ": " + std::to_string(s.files) + (s.files == 1 ? " FILE, " : " FILES, ") +
                Megabytes(s.bytes) + (s.relaunch ? ", RESTARTS THE GAME" : "");
      t.left = s.skipped ? "LATER" : "SKIP";
      t.right = "UPDATE";
      break;
    case State::kDownloading: {
      const std::uint64_t total = s.total != 0 ? s.total : s.bytes;
      const unsigned pct = total == 0 ? 0u : static_cast<unsigned>(s.done * 100 / total);
      t.header = "DOWNLOADING " + v;
      t.body = notes;
      t.body2 = std::to_string(pct) + "%  (" + Megabytes(s.done) + " OF " + Megabytes(total) + ")";
      t.left = "HIDE";
      t.right = "DOWNLOADING " + std::to_string(pct) + "%";
      break;
    }
    case State::kStaged:
      t.header = s.relaunch ? "UPDATE " + v + " READY" : "UPDATE " + v + " APPLIED";
      t.body = notes;
      t.body2 = s.relaunch ? "RESTART ECHO VR TO FINISH THE UPDATE" : "THE NEW CONTENT IS LIVE";
      t.left = s.relaunch ? "LATER" : "CLOSE";
      t.right = s.relaunch ? "RESTART NOW" : "CLOSE";
      break;
    case State::kFailed:
      t.header = "UPDATE " + v + " FAILED";
      t.body = s.error;
      t.body2 = "NOTHING WAS CHANGED";
      t.left = "SKIP";
      t.right = "TRY AGAIN";
      break;
  }
  t.card = Clean(t.card, kCardChars);
  t.header = Clean(t.header, kHeaderChars);
  t.body = Clean(t.body, kLineChars);
  t.body2 = Clean(t.body2, kLineChars);
  t.news = t.body.empty() ? t.body2 : t.body2.empty() ? t.body : t.body + ".  " + t.body2;
  t.news = Clean(t.news, kNewsChars);
  t.left = Clean(t.left, kButtonChars);
  t.right = Clean(t.right, kButtonChars);
  return t;
}

enum class Action { kNone, kStart, kSkip, kRelaunch };

/// The right button (stock CONTINUE): UPDATE / TRY AGAIN / RESTART NOW. kNone means "just close the panel".
inline Action OnRight(const Status& s) {
  if (s.state == State::kOffer || s.state == State::kFailed) return Action::kStart;
  if (s.state == State::kStaged && s.relaunch) return Action::kRelaunch;
  return Action::kNone;
}

/// True when a right click is ours and the panel stays open (it shows the download, or the restart).
inline bool RightKeepsOpen(const Status& s) { return OnRight(s) != Action::kNone || s.state == State::kDownloading; }

/// The left button (stock LEARN MORE): SKIP saves the skip; every left click closes the panel.
inline Action OnLeft(const Status& s) {
  if ((s.state == State::kOffer && !s.skipped) || s.state == State::kFailed) return Action::kSkip;
  return Action::kNone;
}

/// Open the panel by itself when the menu comes up: a new update the player has not skipped.
inline bool OpensByItself(const Status& s) { return s.state == State::kOffer && !s.skipped; }

}  // namespace evr_updates_panel
