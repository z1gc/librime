//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-12-18 GONG Chen <chen.sst@gmail.com>
//
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <rime/gear/ascii_composer.h>
#include <algorithm>

namespace rime {

static struct AsciiModeSwitchStyleDefinition {
  const char* repr;
  AsciiModeSwitchStyle style;
} ascii_mode_switch_styles[] = {{"inline_ascii", kAsciiModeSwitchInline},
                                {"commit_text", kAsciiModeSwitchCommitText},
                                {"commit_code", kAsciiModeSwitchCommitCode},
                                {"clear", kAsciiModeSwitchClear},
                                {NULL, kAsciiModeSwitchNoop}};

static void load_bindings(const an<ConfigMap>& src,
                          AsciiModeSwitchKeyBindings* dest) {
  if (!src)
    return;
  for (auto it = src->begin(); it != src->end(); ++it) {
    auto value = As<ConfigValue>(it->second);
    if (!value)
      continue;
    auto* p = ascii_mode_switch_styles;
    while (p->repr && p->repr != value->str())
      ++p;
    if (p->style == kAsciiModeSwitchNoop)
      continue;
    KeyEvent ke;
    if (!ke.Parse(it->first) || ke.modifier() != 0) {
      LOG(WARNING) << "invalid ascii mode switch key: " << it->first;
      continue;
    }
    // save binding
    (*dest)[ke.keycode()] = p->style;
  }
}

AsciiComposer::AsciiComposer(const Ticket& ticket) : Processor(ticket) {
  LoadConfig(ticket.schema);
}

AsciiComposer::~AsciiComposer() {
  connection_.disconnect();
}

/**
 * Temporary ASCII Mode:
 *
 * 这个模式只是用来快速编辑英文而不需要频繁 shift 切换输入法的一种尝试。
 * 它的设计目的在于保证以中文为主、英文为辅下输入的连贯性。因此，它并不适用于
 * 需要输入大量英文的情况，那种情况下最好的方式就是使用 shift key。
 *
 * 在输入中文后可以直接通过按下空格键进入到临时 ASCII 模式，也可以通过大写字母的
 * 方式进入。直到输入特殊字符（频率最高的应该就是空格了）后解除临时 ASCII 模式。
 *
 * 任意模式下，都可以使用 shift+space 的方式强制输入空格。
 * 目前左 shift 键会无视切换 temp_ascii，右 shift 键则会关闭 temp_ascii 并强制
 * 回到中文模式。这个习惯是否易学，还需要体验一下（。
 *
 * TODO: 空格键作为触发的体验如何？如何重新利用 i 键呢？
 * 感觉 i 可以作为自动填充的 prefix，比如填充云服务器的 sudo 等命令。
 */

static inline void TempAsciiOff(Context* ctx) {
  ctx->set_option("temp_ascii", false);
  ctx->commit_history().clear();
}

// Will clear the commit_history to (try to) avoid inconsistence.
// Maybe it's unneccessary.
static inline void TempAsciiOn(Context* ctx) {
  ctx->set_option("temp_ascii", true);
  ctx->commit_history().clear();
}

// May process the half or full width transform?
static inline bool MayProcessTransform(int ch, bool optional) {
  // Tramsform while in normal and temp_ascii mode; TODO: XK_ macro?
  // ... Or to leave the temp_ascii mode.
  switch (ch) {
    case ',':
    case '^':  // true^false
    case '\\':
    case '"':
    case '!':
    case '?':
    case ';':
      return true;
  }

  // Transform while in normal mode, but not in temp_ascii:
  if (optional) {
    switch (ch) {
      case '.':   // namespace.method, 1.2.3.
      case '\'':  // it's
      case '<':   // 1<3
      case '>':   // pointer->member
      case ':':   // namespace::nested
      case '(':   // invoke()
      case ')':   // revoke()
      case '[':   // a[4]
      case ']':   // b[2]
      case '{':   // {"foh"}
      case '}':   // {"bah"}
        return true;
    }
  }

  // Other keys may enter the temp_ascii mode, and won't be transformed:
  return false;
}

// TODO: Library.
static inline bool IsPrintable(int ch) {
  return XK_space <= ch && ch <= XK_asciitilde;
}

static inline bool IsLower(int ch) {
  return XK_a <= ch && ch <= XK_z;
}

ProcessResult TempAsciiProcess(Context* ctx, const KeyEvent& key_event) {
  const int ch = key_event.keycode();  // actually the `keyval`
  if (key_event.release() || ch == XK_BackSpace || ch == XK_Delete)
    return kNoop;

  // For shift+space, we directly commit it, regardless what mode is:
  const bool composing = ctx->IsComposing();
  if (!composing && ch == XK_space && key_event.shift())
    return kNoop;

  if (ctx->get_option("temp_ascii")) {
    // For XK_Return, we may accidentally disabled the temp mode:
    if (composing)
      return kNoop;

    if (ch == XK_space || !IsPrintable(ch) || MayProcessTransform(ch, false)) {
      // Let other transformer do their's work:
      TempAsciiOff(ctx);
      return kNoop;
    }

    // @see ascii_mode
    return kRejected;
  }

  // Here is !temp_ascii:
  if (composing) {
    // Return key trigger, here we must still composing:
    if (ch == XK_Return) {
      string latest = ctx->commit_history().latest_text();
      if (all_of(latest.cbegin(), latest.cend(), IsPrintable))
        TempAsciiOn(ctx);
    }

    return kNoop;
  } else {
    // Remember here is !temp_ascii, therefore we should consider less keys to
    // turn this mode on, without damanging the type experience.
    // That's why we need a `optional`, and it should be `true`.
    if (IsLower(ch) || !IsPrintable(ch) || MayProcessTransform(ch, true))
      return kNoop;

    // Some other keys like uppercase, +-*/ and more trigger, including space:
    TempAsciiOn(ctx);
    return kRejected;
  }
}

ProcessResult AsciiComposer::ProcessKeyEvent(const KeyEvent& key_event) {
  Context* ctx = engine_->context();
  if ((key_event.shift() && key_event.ctrl()) || key_event.alt() ||
      key_event.super()) {
    shift_key_pressed_ = ctrl_key_pressed_ = false;
    TempAsciiOff(ctx);
    return kNoop;
  }
  if (caps_lock_switch_style_ != kAsciiModeSwitchNoop) {
    ProcessResult result = ProcessCapsLock(key_event);
    if (result != kNoop)
      return result;
  }
  int ch = key_event.keycode();
  if (ch == XK_Eisu_toggle) {  // Alphanumeric toggle
    if (!key_event.release()) {
      shift_key_pressed_ = ctrl_key_pressed_ = false;
      ToggleAsciiModeWithKey(ch);
      return kAccepted;
    } else {
      return kRejected;
    }
  }
  bool is_shift = (ch == XK_Shift_L || ch == XK_Shift_R);
  bool is_ctrl = (ch == XK_Control_L || ch == XK_Control_R);
  if (is_shift || is_ctrl) {
    if (key_event.release()) {
      if (shift_key_pressed_ || ctrl_key_pressed_) {
        auto now = std::chrono::steady_clock::now();
        if (((is_shift && shift_key_pressed_) ||
             (is_ctrl && ctrl_key_pressed_)) &&
            now < toggle_expired_) {
          TempAsciiOff(ctx);
          if (ch == XK_Shift_R)
            SwitchAsciiMode(false, AsciiModeSwitchStyle::kAsciiModeSwitchNoop);
          else
            ToggleAsciiModeWithKey(ch);
        }
        shift_key_pressed_ = ctrl_key_pressed_ = false;
        return kNoop;
      }
    } else if (!(shift_key_pressed_ || ctrl_key_pressed_)) {  // first key down
      if (is_shift)
        shift_key_pressed_ = true;
      else {
        // Maybe a ctrl+ shortcut, reset the temp_ascii:
        TempAsciiOff(ctx);
        ctrl_key_pressed_ = true;
      }
      // will not toggle unless the toggle key is released shortly
      const auto toggle_duration_limit = std::chrono::milliseconds(500);
      auto now = std::chrono::steady_clock::now();
      toggle_expired_ = now + toggle_duration_limit;
    }
    return kNoop;
  }
  // other keys
  shift_key_pressed_ = ctrl_key_pressed_ = false;
  bool ascii_mode = ctx->get_option("ascii_mode");
  if (ascii_mode) {
    if (!ctx->IsComposing()) {
      return kRejected;  // direct commit
    }
    // edit inline ascii string
    if (!key_event.release() && ch >= 0x20 && ch < 0x80) {
      ctx->PushInput(ch);
      return kAccepted;
    }
  }
  return TempAsciiProcess(ctx, key_event);
}

ProcessResult AsciiComposer::ProcessCapsLock(const KeyEvent& key_event) {
  int ch = key_event.keycode();
  if (ch == XK_Caps_Lock) {
    if (!key_event.release()) {
      shift_key_pressed_ = ctrl_key_pressed_ = false;
      // temporarily disable good-old (uppercase) Caps Lock as mode switch key
      // in case the user switched to ascii mode with other keys, eg. with Shift
      if (good_old_caps_lock_ && !toggle_with_caps_) {
        Context* ctx = engine_->context();
        bool ascii_mode = ctx->get_option("ascii_mode");
        if (ascii_mode) {
          return kRejected;
        }
      }
      toggle_with_caps_ = !key_event.caps();
      // NOTE: for Linux, Caps Lock modifier is clear when we are about to
      // turn it on; for Windows it is the opposite:
      // Caps Lock modifier has been set before we process VK_CAPITAL.
      // here we assume IBus' behavior and invert caps with ! operation.
      SwitchAsciiMode(!key_event.caps(), caps_lock_switch_style_);
      return kAccepted;
    } else {
      return kRejected;
    }
  }
  if (key_event.caps()) {
    if (!good_old_caps_lock_ && !key_event.release() && !key_event.ctrl() &&
        isascii(ch) && isalpha(ch)) {
      // output ascii characters ignoring Caps Lock
      if (islower(ch))
        ch = toupper(ch);
      else if (isupper(ch))
        ch = tolower(ch);
      engine_->CommitText(string(1, ch));
      return kAccepted;
    } else {
      return kRejected;
    }
  }
  return kNoop;
}

void AsciiComposer::LoadConfig(Schema* schema) {
  bindings_.clear();
  caps_lock_switch_style_ = kAsciiModeSwitchNoop;
  good_old_caps_lock_ = false;
  if (!schema)
    return;
  Config* config = schema->config();
  the<Config> preset_config(Config::Require("config")->Create("default"));
  if (!config->GetBool("ascii_composer/good_old_caps_lock",
                       &good_old_caps_lock_)) {
    if (preset_config) {
      preset_config->GetBool("ascii_composer/good_old_caps_lock",
                             &good_old_caps_lock_);
    }
  }
  if (auto bindings = config->GetMap("ascii_composer/switch_key")) {
    load_bindings(bindings, &bindings_);
  } else if (auto bindings = preset_config ? preset_config->GetMap(
                                                 "ascii_composer/switch_key")
                                           : nullptr) {
    load_bindings(bindings, &bindings_);
  } else {
    LOG(ERROR) << "Missing ascii bindings.";
    return;
  }
  auto it = bindings_.find(XK_Caps_Lock);
  if (it != bindings_.end()) {
    caps_lock_switch_style_ = it->second;
    if (caps_lock_switch_style_ == kAsciiModeSwitchInline) {  // can't do that
      caps_lock_switch_style_ = kAsciiModeSwitchClear;
    }
  }
}

bool AsciiComposer::ToggleAsciiModeWithKey(int key_code) {
  auto it = bindings_.find(key_code);
  if (it == bindings_.end())
    return false;
  AsciiModeSwitchStyle style = it->second;
  Context* ctx = engine_->context();
  bool ascii_mode = !ctx->get_option("ascii_mode");
  SwitchAsciiMode(ascii_mode, style);
  toggle_with_caps_ = (key_code == XK_Caps_Lock);
  return true;
}

void AsciiComposer::SwitchAsciiMode(bool ascii_mode,
                                    AsciiModeSwitchStyle style) {
  DLOG(INFO) << "ascii mode: " << ascii_mode << ", switch style: " << style;
  Context* ctx = engine_->context();
  if (ctx->IsComposing()) {
    connection_.disconnect();
    // temporary ascii mode in desired manner
    if (style == kAsciiModeSwitchInline) {
      LOG(INFO) << "converting current composition to "
                << (ascii_mode ? "ascii" : "non-ascii") << " mode.";
      if (ascii_mode) {
        connection_ = ctx->update_notifier().connect(
            [this](Context* ctx) { OnContextUpdate(ctx); });
      }
    } else if (style == kAsciiModeSwitchCommitText) {
      ctx->ConfirmCurrentSelection();
    } else if (style == kAsciiModeSwitchCommitCode) {
      ctx->ClearNonConfirmedComposition();
      ctx->Commit();
    } else if (style == kAsciiModeSwitchClear) {
      ctx->Clear();
    }
  }
  // refresh non-confirmed composition with new mode
  ctx->set_option("ascii_mode", ascii_mode);
}

void AsciiComposer::OnContextUpdate(Context* ctx) {
  if (!ctx->IsComposing()) {
    connection_.disconnect();
    // quit temporary ascii mode
    ctx->set_option("ascii_mode", false);
  }
}

}  // namespace rime
