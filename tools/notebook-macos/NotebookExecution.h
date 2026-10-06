#pragma once
#include "tools/iamber/tabs.h"
#include "tools/notebook-worker/process.h"
#include "tools/notebook-worker/presentation.h"
#include <map>
#include <memory>
#include <mutex>

// Independent lifetime, no Session/UI pointers. Only this small control object
// is shared with the main thread while the bridge owner waits for IPC.
class NotebookExecutionControl {
public:
  int phase() const;
  bool stop(bool force);
  void begin();
  void bind(std::shared_ptr<amber::notebook::worker::Process> process);
  void running(amber::notebook::worker::RequestId request);
  void draining();
  bool forced() const;
  bool commit();
  void finish();
  void publish_live(amber::notebook::worker::Presentation);
  std::shared_ptr<const amber::notebook::worker::Presentation> take_live();
private:
  mutable std::mutex mutex_;
  // 0 idle, 1 preparing, 2 running, 3 draining, 4 stopping, 5 terminating.
  int phase_ = 0;
  bool stop_ = false, force_ = false;
  std::shared_ptr<amber::notebook::worker::Process> process_;
  amber::notebook::worker::RequestId request_;
  std::uint64_t generation_ = 0;
  std::shared_ptr<const amber::notebook::worker::Presentation> live_;
};

// Owner-queue only. One persistent worker per sheet; host Sessions remain
// source/presentation containers and never construct a local VM in this mode.
class NotebookExecution {
public:
  explicit NotebookExecution(std::string executable) : executable_(std::move(executable)) {}
  std::shared_ptr<NotebookExecutionControl> control = std::make_shared<NotebookExecutionControl>();
  void run(ProjectTabs &, ProjectTab &, const std::string &action, std::size_t index);
  void restart(ProjectTab &);
  bool native_trusted = false;
private:
  struct Sheet {
    std::shared_ptr<amber::notebook::worker::Process> process;
    bool interrupted = false;
  };
  std::string executable_;
  std::map<std::string, Sheet> sheets_;
};
