// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/ui_dart_state.h"

#include <iostream>

#include "flutter/fml/message_loop.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_message_handler.h"

#if defined(OS_ANDROID)
#include <android/log.h>
#elif defined(OS_IOS)
extern "C" {
// Cannot import the syslog.h header directly because of macro collision.
extern void syslog(int, const char*, ...);
}
#endif

using tonic::ToDart;

namespace flutter {

UIDartState::UIDartState(
    TaskRunners task_runners,
    TaskObserverAdd add_callback,
    TaskObserverRemove remove_callback,
    fml::WeakPtr<SnapshotDelegate> snapshot_delegate,
    fml::WeakPtr<HintFreedDelegate> hint_freed_delegate,
    fml::WeakPtr<IOManager> io_manager,
    fml::RefPtr<SkiaUnrefQueue> skia_unref_queue,
    fml::WeakPtr<ImageDecoder> image_decoder,
    fml::WeakPtr<ImageGeneratorRegistry> image_generator_registry,
    std::string advisory_script_uri,
    std::string advisory_script_entrypoint,
    std::string logger_prefix,
    UnhandledExceptionCallback unhandled_exception_callback,
    LogMessageCallback log_message_callback,
    std::shared_ptr<IsolateNameServer> isolate_name_server,
    bool is_root_isolate,
    std::shared_ptr<VolatilePathTracker> volatile_path_tracker,
    bool enable_skparagraph)
    : task_runners_(std::move(task_runners)),
      add_callback_(std::move(add_callback)),
      remove_callback_(std::move(remove_callback)),
      snapshot_delegate_(std::move(snapshot_delegate)),
      hint_freed_delegate_(std::move(hint_freed_delegate)),
      io_manager_(std::move(io_manager)),
      skia_unref_queue_(std::move(skia_unref_queue)),
      image_decoder_(std::move(image_decoder)),
      image_generator_registry_(std::move(image_generator_registry)),
      volatile_path_tracker_(std::move(volatile_path_tracker)),
      advisory_script_uri_(std::move(advisory_script_uri)),
      advisory_script_entrypoint_(std::move(advisory_script_entrypoint)),
      logger_prefix_(std::move(logger_prefix)),
      is_root_isolate_(is_root_isolate),
      unhandled_exception_callback_(unhandled_exception_callback),
      log_message_callback_(log_message_callback),
      isolate_name_server_(std::move(isolate_name_server)),
      enable_skparagraph_(enable_skparagraph) {
  AddOrRemoveTaskObserver(true /* add */);
}

UIDartState::~UIDartState() {
  AddOrRemoveTaskObserver(false /* remove */);
}

const std::string& UIDartState::GetAdvisoryScriptURI() const {
  return advisory_script_uri_;
}

const std::string& UIDartState::GetAdvisoryScriptEntrypoint() const {
  return advisory_script_entrypoint_;
}

void UIDartState::DidSetIsolate() {
  main_port_ = Dart_GetMainPortId();
  std::ostringstream debug_name;
  // main.dart$main-1234
  debug_name << advisory_script_uri_ << "$" << advisory_script_entrypoint_
             << "-" << main_port_;
  SetDebugName(debug_name.str());
}

void UIDartState::ThrowIfUIOperationsProhibited() {
  if (!UIDartState::Current()->IsRootIsolate()) {
    Dart_ThrowException(
        tonic::ToDart("UI actions are only available on root isolate."));
  }
}

void UIDartState::SetDebugName(const std::string debug_name) {
  debug_name_ = debug_name;
  if (platform_configuration_) {
    platform_configuration_->client()->UpdateIsolateDescription(debug_name_,
                                                                main_port_);
  }
}

UIDartState* UIDartState::Current() {
  return static_cast<UIDartState*>(DartState::Current());
}

void UIDartState::SetPlatformConfiguration(
    std::unique_ptr<PlatformConfiguration> platform_configuration) {
  platform_configuration_ = std::move(platform_configuration);
  if (platform_configuration_) {
    platform_configuration_->client()->UpdateIsolateDescription(debug_name_,
                                                                main_port_);
  }
}

const TaskRunners& UIDartState::GetTaskRunners() const {
  return task_runners_;
}

fml::WeakPtr<IOManager> UIDartState::GetIOManager() const {
  return io_manager_;
}

fml::RefPtr<flutter::SkiaUnrefQueue> UIDartState::GetSkiaUnrefQueue() const {
  return skia_unref_queue_;
}

std::shared_ptr<VolatilePathTracker> UIDartState::GetVolatilePathTracker()
    const {
  return volatile_path_tracker_;
}

void UIDartState::ScheduleMicrotask(Dart_Handle closure) {
  if (tonic::LogIfError(closure) || !Dart_IsClosure(closure)) {
    return;
  }

  microtask_queue_.ScheduleMicrotask(closure);
}

void UIDartState::FlushMicrotasksNow() {
  microtask_queue_.RunMicrotasks();
}

void UIDartState::AddOrRemoveTaskObserver(bool add) {
  auto task_runner = task_runners_.GetUITaskRunner();
  if (!task_runner) {
    // This may happen in case the isolate has no thread affinity (for example,
    // the service isolate).
    return;
  }
  FML_DCHECK(add_callback_ && remove_callback_);
  if (add) {
    add_callback_(reinterpret_cast<intptr_t>(this),
                  [this]() { this->FlushMicrotasksNow(); });
  } else {
    remove_callback_(reinterpret_cast<intptr_t>(this));
  }
}

fml::WeakPtr<SnapshotDelegate> UIDartState::GetSnapshotDelegate() const {
  return snapshot_delegate_;
}

fml::WeakPtr<HintFreedDelegate> UIDartState::GetHintFreedDelegate() const {
  return hint_freed_delegate_;
}

fml::WeakPtr<GrDirectContext> UIDartState::GetResourceContext() const {
  if (!io_manager_) {
    return {};
  }
  return io_manager_->GetResourceContext();
}

fml::WeakPtr<ImageDecoder> UIDartState::GetImageDecoder() const {
  return image_decoder_;
}

fml::WeakPtr<ImageGeneratorRegistry> UIDartState::GetImageGeneratorRegistry()
    const {
  return image_generator_registry_;
}

std::shared_ptr<IsolateNameServer> UIDartState::GetIsolateNameServer() const {
  return isolate_name_server_;
}

tonic::DartErrorHandleType UIDartState::GetLastError() {
  tonic::DartErrorHandleType error = message_handler().isolate_last_error();
  if (error == tonic::kNoError) {
    error = microtask_queue_.GetLastError();
  }
  return error;
}

void UIDartState::ReportUnhandledException(const std::string& error,
                                           const std::string& stack_trace) {
  if (unhandled_exception_callback_ &&
      unhandled_exception_callback_(error, stack_trace)) {
    return;
  }

  // Either the exception handler was not set or it could not handle the error,
  // just log the exception.
  FML_LOG(ERROR) << "Unhandled Exception: " << error << std::endl
                 << stack_trace;
}

void UIDartState::LogMessage(const std::string& tag,
                             const std::string& message) const {
  if (log_message_callback_) {
    log_message_callback_(tag, message);
  } else {
    // Fall back to previous behavior if unspecified.
#if defined(OS_ANDROID)
    __android_log_print(ANDROID_LOG_INFO, tag.c_str(), "%.*s",
                        (int)message.size(), message.c_str());
#elif defined(OS_IOS)
    std::stringstream stream;
    if (tag.size() > 0) {
      stream << tag << ": ";
    }
    stream << message;
    std::string log = stream.str();
    syslog(1 /* LOG_ALERT */, "%.*s", (int)log.size(), log.c_str());
#else
    if (tag.size() > 0) {
      std::cout << tag << ": ";
    }
    std::cout << message << std::endl;
#endif
  }
}

bool UIDartState::enable_skparagraph() const {
  return enable_skparagraph_;
}

}  // namespace flutter
