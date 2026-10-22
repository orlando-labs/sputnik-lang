package sdk.digest

export SHA256

native class SHA256 from "sdk.digest.SHA256" owned:
  def init() from "sphash_new"
  def update(data as Bytes) from "sphash_update"
  def finish() -> Str from "sphash_finish"
  def destroy!() from "sphash_free"
