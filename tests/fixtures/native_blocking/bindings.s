package test.blocking.bindings

export worker, inline_worker, batch, fail, context, cancel_wait, cancel_counts, Probe

native def worker() from "blocking.worker" blocking
native def inline_worker() from "blocking.inline_worker"
native def batch(callback) from "blocking.batch" blocking
native def fail() from "blocking.fail" blocking
native def context() from "blocking.context"
native def cancel_wait() from "blocking.cancel_wait" blocking
native def cancel_counts() from "blocking.cancel_counts"

native class Probe from "blocking.Probe" borrowed:
  def init() from "blocking.probe_new" blocking
  def worker() from "blocking.probe_worker" blocking
  def constructed_on_worker() from "blocking.probe_constructed"
  def inline_worker() from "blocking.probe_inline_worker"
