/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/emulator_window.h"

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/system.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

// Autogenerated by `xb premake`.
#include "build/version.h"

DECLARE_bool(debug);

namespace xe {
namespace app {

using xe::ui::FileDropEvent;
using xe::ui::KeyEvent;
using xe::ui::MenuItem;
using xe::ui::MouseEvent;
using xe::ui::UIEvent;

const std::string kBaseTitle = "xenia-canary";

EmulatorWindow::EmulatorWindow(Emulator* emulator)
    : emulator_(emulator),
      loop_(ui::Loop::Create()),
      window_(ui::Window::Create(loop_.get(), kBaseTitle)) {
  base_title_ = kBaseTitle +
#ifdef DEBUG
#if _NO_DEBUG_HEAP == 1
                " DEBUG"
#else
                " CHECKED"
#endif
#endif
                " (" XE_BUILD_BRANCH "/" XE_BUILD_COMMIT_SHORT "/" XE_BUILD_DATE
                ")";
}

EmulatorWindow::~EmulatorWindow() {
  loop_->PostSynchronous([this]() { window_.reset(); });
}

std::unique_ptr<EmulatorWindow> EmulatorWindow::Create(Emulator* emulator) {
  std::unique_ptr<EmulatorWindow> emulator_window(new EmulatorWindow(emulator));

  emulator_window->loop()->PostSynchronous([&emulator_window]() {
    xe::threading::set_name("Windowing Loop");
    xe::Profiler::ThreadEnter("Windowing Loop");

    if (!emulator_window->Initialize()) {
      xe::FatalError("Failed to initialize main window");
      return;
    }
  });

  return emulator_window;
}

bool EmulatorWindow::Initialize() {
  if (!window_->Initialize()) {
    XELOGE("Failed to initialize platform window");
    return false;
  }

  UpdateTitle();

  window_->on_closed.AddListener([this](UIEvent* e) { loop_->Quit(); });
  loop_->on_quit.AddListener([this](UIEvent* e) { window_.reset(); });

  window_->on_file_drop.AddListener(
      [this](FileDropEvent* e) { FileDrop(e->filename()); });

  window_->on_key_down.AddListener([this](KeyEvent* e) {
    bool handled = true;
    switch (e->key_code()) {
      case 0x4F: {  // o
        if (e->is_ctrl_pressed()) {
          FileOpen();
        }
      } break;
      case 0x6A: {  // numpad *
        CpuTimeScalarReset();
      } break;
      case 0x6D: {  // numpad minus
        CpuTimeScalarSetHalf();
      } break;
      case 0x6B: {  // numpad plus
        CpuTimeScalarSetDouble();
      } break;

      case 0x72: {  // F3
        Profiler::ToggleDisplay();
      } break;

      case 0x73: {  // VK_F4
        GpuTraceFrame();
      } break;
      case 0x74: {  // VK_F5
        GpuClearCaches();
      } break;
      case 0x76: {  // VK_F7
        // Save to file
        // TODO: Choose path based on user input, or from options
        // TODO: Spawn a new thread to do this.
        emulator()->SaveToFile("test.sav");
      } break;
      case 0x77: {  // VK_F8
        // Restore from file
        // TODO: Choose path from user
        // TODO: Spawn a new thread to do this.
        emulator()->RestoreFromFile("test.sav");
      } break;
      case 0x7A: {  // VK_F11
        ToggleFullscreen();
      } break;
      case 0x1B: {  // VK_ESCAPE
                    // Allow users to escape fullscreen (but not enter it).
        if (window_->is_fullscreen()) {
          window_->ToggleFullscreen(false);
        } else {
          handled = false;
        }
      } break;

      case 0x13: {  // VK_PAUSE
        CpuBreakIntoDebugger();
      } break;
      case 0x03: {  // VK_CANCEL
        CpuBreakIntoHostDebugger();
      } break;

      case 0x70: {  // VK_F1
        ShowHelpWebsite();
      } break;

      case 0x71: {  // VK_F2
        ShowCommitID();
      } break;

      default: {
        handled = false;
      } break;
    }
    e->set_handled(handled);
  });

  window_->on_mouse_move.AddListener([this](MouseEvent* e) {
    if (window_->is_fullscreen() && (e->dx() > 2 || e->dy() > 2)) {
      if (!window_->is_cursor_visible()) {
        window_->set_cursor_visible(true);
      }

      cursor_hide_time_ = Clock::QueryHostSystemTime() + 30000000;
    }

    e->set_handled(false);
  });

  window_->on_paint.AddListener([this](UIEvent* e) { CheckHideCursor(); });

  // Main menu.
  // FIXME: This code is really messy.
  auto main_menu = MenuItem::Create(MenuItem::Type::kNormal);
  auto file_menu = MenuItem::Create(MenuItem::Type::kPopup, "&File");
  {
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Open...", "Ctrl+O",
                         std::bind(&EmulatorWindow::FileOpen, this)));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Close",
                         std::bind(&EmulatorWindow::FileClose, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Show content directory...",
        std::bind(&EmulatorWindow::ShowContentDirectory, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kString, "E&xit",
                                         "Alt+F4",
                                         [this]() { window_->Close(); }));
  }
  main_menu->AddChild(std::move(file_menu));

  // CPU menu.
  auto cpu_menu = MenuItem::Create(MenuItem::Type::kPopup, "&CPU");
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Reset Time Scalar", "Numpad *",
        std::bind(&EmulatorWindow::CpuTimeScalarReset, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Time Scalar /= 2", "Numpad -",
        std::bind(&EmulatorWindow::CpuTimeScalarSetHalf, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Time Scalar *= 2", "Numpad +",
        std::bind(&EmulatorWindow::CpuTimeScalarSetDouble, this)));
  }
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "Toggle Profiler &Display", "F3",
                                        []() { Profiler::ToggleDisplay(); }));
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "&Pause/Resume Profiler", "`",
                                        []() { Profiler::TogglePause(); }));
  }
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break and Show Guest Debugger",
        "Pause/Break", std::bind(&EmulatorWindow::CpuBreakIntoDebugger, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break into Host Debugger",
        "Ctrl+Pause/Break",
        std::bind(&EmulatorWindow::CpuBreakIntoHostDebugger, this)));
  }
  main_menu->AddChild(std::move(cpu_menu));

  // GPU menu.
  auto gpu_menu = MenuItem::Create(MenuItem::Type::kPopup, "&GPU");
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Trace Frame", "F4",
                         std::bind(&EmulatorWindow::GpuTraceFrame, this)));
  }
  gpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Clear Runtime Caches", "F5",
                         std::bind(&EmulatorWindow::GpuClearCaches, this)));
  }
  main_menu->AddChild(std::move(gpu_menu));

  // Window menu.
  auto window_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Window");
  {
    window_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Fullscreen", "F11",
                         std::bind(&EmulatorWindow::ToggleFullscreen, this)));
  }
  main_menu->AddChild(std::move(window_menu));

  // Help menu.
  auto help_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Help");
  {
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Build commit on GitHub...",
                         "F2", std::bind(&EmulatorWindow::ShowCommitID, this)));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Recent changes on GitHub...", [this]() {
          LaunchWebBrowser(
              "https://github.com/xenia-project/xenia/compare/" XE_BUILD_COMMIT
              "..." XE_BUILD_BRANCH);
        }));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Website...", "F1",
                         std::bind(&EmulatorWindow::ShowHelpWebsite, this)));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&About...",
        [this]() { LaunchWebBrowser("https://xenia.jp/about/"); }));
  }
  main_menu->AddChild(std::move(help_menu));

  window_->set_main_menu(std::move(main_menu));

  window_->Resize(1280, 720);

  window_->DisableMainMenu();

  return true;
}

void EmulatorWindow::FileDrop(const std::filesystem::path& filename) {
  auto result = emulator_->LaunchPath(filename);
  if (XFAILED(result)) {
    // TODO: Display a message box.
    XELOGE("Failed to launch target: {:08X}", result);
  }
}

void EmulatorWindow::FileOpen() {
  std::filesystem::path path;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title("Select Content Package");
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.xcp;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Xbox Executable (*.xex)", "*.xex"},
      //{"Content Package (*.xcp)", "*.xcp" },
      {"All Files (*.*)", "*.*"},
  });
  if (file_picker->Show(window_->native_handle())) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
  }

  if (!path.empty()) {
    // Normalize the path and make absolute.
    auto abs_path = std::filesystem::absolute(path);
    auto result = emulator_->LaunchPath(abs_path);
    if (XFAILED(result)) {
      // TODO: Display a message box.
      XELOGE("Failed to launch target: {:08X}", result);
    }
  }
}

void EmulatorWindow::FileClose() {
  if (emulator_->is_title_open()) {
    emulator_->TerminateTitle();
  }
}

void EmulatorWindow::ShowContentDirectory() {
  std::filesystem::path target_path;

  auto content_root = emulator_->content_root();
  if (!emulator_->is_title_open() || !emulator_->kernel_state()) {
    target_path = content_root;
  } else {
    // TODO(gibbed): expose this via ContentManager?
    auto title_id =
        fmt::format("{:08X}", emulator_->kernel_state()->title_id());
    auto package_root = content_root / title_id;
    target_path = package_root;
  }

  if (!std::filesystem::exists(target_path)) {
    std::filesystem::create_directories(target_path);
  }

  LaunchFileExplorer(target_path);
}

void EmulatorWindow::CheckHideCursor() {
  if (!window_->is_fullscreen()) {
    // Only hide when fullscreen.
    return;
  }

  if (Clock::QueryHostSystemTime() > cursor_hide_time_) {
    window_->set_cursor_visible(false);
  }
}

void EmulatorWindow::CpuTimeScalarReset() {
  Clock::set_guest_time_scalar(1.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetHalf() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() / 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetDouble() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() * 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuBreakIntoDebugger() {
  if (!cvars::debug) {
    xe::ui::ImGuiDialog::ShowMessageBox(window_.get(), "Xenia Debugger",
                                        "Xenia must be launched with the "
                                        "--debug flag in order to enable "
                                        "debugging.");
    return;
  }
  auto processor = emulator()->processor();
  if (processor->execution_state() == cpu::ExecutionState::kRunning) {
    // Currently running, so interrupt (and show the debugger).
    processor->Pause();
  } else {
    // Not running, so just bring the debugger into focus.
    processor->ShowDebugger();
  }
}

void EmulatorWindow::CpuBreakIntoHostDebugger() { xe::debugging::Break(); }

void EmulatorWindow::GpuTraceFrame() {
  emulator()->graphics_system()->RequestFrameTrace();
}

void EmulatorWindow::GpuClearCaches() {
  emulator()->graphics_system()->ClearCaches();
}

void EmulatorWindow::ToggleFullscreen() {
  window_->ToggleFullscreen(!window_->is_fullscreen());

  // Hide the cursor after a second if we're going fullscreen
  cursor_hide_time_ = Clock::QueryHostSystemTime() + 30000000;
  if (!window_->is_fullscreen()) {
    window_->set_cursor_visible(true);
  }
}

void EmulatorWindow::ShowHelpWebsite() { LaunchWebBrowser("https://xenia.jp"); }

void EmulatorWindow::ShowCommitID() {
  LaunchWebBrowser(
      "https://github.com/xenia-project/xenia/commit/" XE_BUILD_COMMIT "/");
}

void EmulatorWindow::UpdateTitle() {
  std::string title(base_title_);

  if (emulator()->is_title_open()) {
    auto game_title = emulator()->game_title();
    title += fmt::format(" | [{:08X}] {}", emulator()->title_id(), game_title);
  }

  auto graphics_system = emulator()->graphics_system();
  if (graphics_system) {
    auto graphics_name = graphics_system->name();
    title += fmt::format(" <{}>", graphics_name);
  }

  if (Clock::guest_time_scalar() != 1.0) {
    title += fmt::format(" (@{:.2f}x)", Clock::guest_time_scalar());
  }

  if (initializing_shader_storage_) {
    title +=
        " (Preloading shaders"
        u8"\u2026"
        ")";
  }

  patcher::PatchingSystem* patching_system = emulator()->patching_system();
  if (patching_system) {
    auto title_patched =
        patching_system->IsAnyPatchApplied() ? " [Patches Applied]" : "";
    title += title_patched;
  }
  window_->set_title(title);
}

void EmulatorWindow::SetInitializingShaderStorage(bool initializing) {
  if (initializing_shader_storage_ == initializing) {
    return;
  }
  initializing_shader_storage_ = initializing;
  UpdateTitle();
}

}  // namespace app
}  // namespace xe
