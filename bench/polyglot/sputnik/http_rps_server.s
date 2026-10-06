package sputnik.bench.polyglot.http_rps_server

import ember
import orm
import task
from ember.telemetry import MetricsRegistry, Telemetry
from ember.integrations.orm import pool as orm_pool
from orm.sqlite3 import memory_pool

export main

def bounded_text?(value, minimum, maximum):
  Str === value and value.length >= minimum and value.length <= maximum

def upper_ascii?(value):
  return false unless Str === value
  value.chars.all? |char|: "ABCDEFGHIJKLMNOPQRSTUVWXYZ".contains?(char)

def valid_sku?(value):
  return false unless bounded_text?(value, 3, 64)
  value.chars.all? |char|:
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_".contains?(char)

def catalog_category?(value):
  Str === value and ["hardware", "software", "service", "accessory"].include?(value)

def catalog_status?(value):
  Str === value and ["draft", "active", "paused", "retired"].include?(value)

def integer_between?(value, minimum, maximum):
  Int === value and value >= minimum and value <= maximum

def valid_barcode?(value):
  return false unless bounded_text?(value, 8, 32)
  value.chars.all? |char|: "0123456789".contains?(char)

def valid_tags_json?(value):
  return false unless Str === value
  try:
    tags = Json.parse(value)
    Array === tags and tags.count <= 16
      and tags.all? |tag|: bounded_text?(tag, 1, 32)
  rescue Exception |error|:
    false

def valid_metadata_json?(value):
  return false unless Str === value
  try:
    metadata = Json.parse(value)
    Map === metadata and metadata.count == 3
      and bounded_text?(metadata[:source], 1, 64)
      and bounded_text?(metadata[:batch], 1, 64)
      and Bool === metadata[:fragile]
  rescue Exception |error|:
    false

class CatalogItem:
  use orm.model(table: :catalog_items):
    validate:
      expect(:sku, valid_sku?(sku), code: :invalid_format)
      expect(:name, bounded_text?(name, 1, 160), code: :invalid_length)
      expect(:description, bounded_text?(description, 1, 2000), code: :invalid_length)
      expect(:category, catalog_category?(category), code: :unsupported)
      expect(:status, catalog_status?(status), code: :unsupported)
      expect(:price_cents, integer_between?(price_cents, 0, 1000000000), code: :out_of_range)
      expect(:currency, Str === currency and currency.length == 3 and upper_ascii?(currency), code: :invalid_format)
      expect(:stock_count, integer_between?(stock_count, 0, 1000000), code: :out_of_range)
      expect(:weight_grams, integer_between?(weight_grams, 1, 10000000), code: :out_of_range)
      expect(:active, active == 0 or active == 1, code: :invalid)
      expect(:manufacturer, bounded_text?(manufacturer, 1, 120), code: :invalid_length)
      expect(:country_code, Str === country_code and country_code.length == 2 and upper_ascii?(country_code), code: :invalid_format)
      expect(:barcode, valid_barcode?(barcode), code: :invalid_format)
      expect(:color, bounded_text?(color, 1, 40), code: :invalid_length)
      expect(:size, bounded_text?(size, 1, 40), code: :invalid_length)
      expect(:rating_milli, integer_between?(rating_milli, 0, 5000), code: :out_of_range)
      expect(:tags_json, valid_tags_json?(tags_json), code: :invalid)
      expect(:metadata_json, valid_metadata_json?(metadata_json), code: :invalid)

class Schemas:
  attr create_item
  attr patch_item
  attr list_item

  def init():
    @create_item = ember.params(root: :item, unknown: :reject):
      field(:sku, required: true)
      field(:name, required: true)
      field(:description, required: true)
      field(:category, required: true)
      field(:status, required: true)
      field(:price_cents, type: Int, required: true)
      field(:currency, required: true)
      field(:stock_count, type: Int, required: true)
      field(:weight_grams, type: Int, required: true)
      field(:active, type: Bool, required: true)
      field(:manufacturer, required: true)
      field(:country_code, required: true)
      field(:barcode, required: true)
      field(:color, required: true)
      field(:size, required: true)
      field(:rating_milli, type: Int, required: true)
      array(:tags, of: Str, required: true)
      object(:metadata, required: true, unknown: :reject):
        field(:source, required: true)
        field(:batch, required: true)
        field(:fragile, type: Bool, required: true)

    @patch_item = ember.params(root: :item, unknown: :reject):
      field(:sku)
      field(:name)
      field(:description)
      field(:category)
      field(:status)
      field(:price_cents, type: Int)
      field(:currency)
      field(:stock_count, type: Int)
      field(:weight_grams, type: Int)
      field(:active, type: Bool)
      field(:manufacturer)
      field(:country_code)
      field(:barcode)
      field(:color)
      field(:size)
      field(:rating_milli, type: Int)
      array(:tags, of: Str)
      object(:metadata, unknown: :reject):
        field(:source, required: true)
        field(:batch, required: true)
        field(:fragile, type: Bool, required: true)

    @list_item = ember.params(unknown: :reject):
      field(:status)

def problem(status, code, message, details: []):
  ember.render(
    json: {error: {code: code, message: message, details: details}},
    status: status)

def validation_problem(issue):
  problem(
    422,
    "validation_failed",
    "catalog item is invalid",
    details: [{field: issue[0], code: issue[1]}])

def storage_attrs(attrs):
  result = {}
  [
    :sku, :name, :description, :category, :status, :price_cents, :currency,
    :stock_count, :weight_grams, :manufacturer, :country_code, :barcode,
    :color, :size, :rating_milli
  ].each |name|:
    result[name] = attrs[name] if attrs.has_key?(name)
  if attrs.has_key?(:active):
    result[:active] = if attrs[:active] then 1 else 0
  result[:tags_json] = Json.generate(attrs[:tags]) if attrs.has_key?(:tags)
  result[:metadata_json] = Json.generate(attrs[:metadata]) if attrs.has_key?(:metadata)
  result

def item_json(item):
  {
    id: item[:id],
    sku: item[:sku],
    name: item[:name],
    description: item[:description],
    category: item[:category],
    status: item[:status],
    price_cents: item[:price_cents],
    currency: item[:currency],
    stock_count: item[:stock_count],
    weight_grams: item[:weight_grams],
    active: item[:active] == 1,
    manufacturer: item[:manufacturer],
    country_code: item[:country_code],
    barcode: item[:barcode],
    color: item[:color],
    size: item[:size],
    rating_milli: item[:rating_milli],
    tags: Json.parse(item[:tags_json]),
    metadata: Json.parse(item[:metadata_json]),
    version: item[:version],
    created_at: item[:created_at],
    updated_at: item[:updated_at]
  }

def find_item(id):
  result = CatalogItem.find(id)
  return result if result.ok?
  return Err(problem(404, "not_found", "catalog item was not found")) if orm.NotFoundError === result.error
  Err(problem(503, "storage_unavailable", "catalog storage is unavailable"))

def requested_version(request):
  raw = request.header("if-match")
  return Err(problem(428, "precondition_required", "If-Match is required")) unless raw
  text = raw.trim
  valid = text.present? and text.length <= 18
  valid = valid and text.chars.all? |char|: "0123456789".contains?(char)
  return Err(problem(400, "invalid_precondition", "If-Match must be a positive integer")) unless valid
  version = text.to_int()
  return Err(problem(400, "invalid_precondition", "If-Match must be a positive integer")) unless version > 0
  Ok(version)

def validation_details(error):
  error.errors.issues.map |issue|:
    field = issue.attribute.to_str
    field = "tags" if issue.attribute == :tags_json
    field = "metadata" if issue.attribute == :metadata_json
    {field: field, code: issue.code.to_str}

def persistence_problem(result):
  if orm.ValidationError === result.error:
    return problem(
      422,
      "validation_failed",
      "catalog item is invalid",
      details: validation_details(result.error))
  message = result.error.message.downcase
  if message.contains?("constraint") or message.contains?("unique"):
    return problem(409, "sku_conflict", "catalog item SKU already exists")
  problem(503, "storage_unavailable", "catalog storage is unavailable")

def update_item(item, attrs, request, partial: false):
  version = requested_version(request)
  return version.error if version.err?
  return problem(409, "version_conflict", "catalog item version is stale") unless version.value == item[:version]
  if partial and attrs.empty?:
    return problem(
      422,
      "validation_failed",
      "catalog item is invalid",
      details: [{field: "item", code: "empty"}])

  item.assign!(storage_attrs(attrs))
  item[:version] = item[:version] + 1
  item[:updated_at] = Time.now.iso8601
  saved = item.save
  return persistence_problem(saved) if saved.err?
  ember.render(
    json: item_json(saved.value),
    headers: {"etag": saved.value[:version].to_str})

class Items < ember.Controller:
  def index():
    filters = params.query.expect(service(:schemas).list_item)
    if filters.has_key?(:status) and not catalog_status?(filters[:status]):
      return problem(
        422,
        "validation_failed",
        "catalog filter is invalid",
        details: [{field: "status", code: "unsupported"}])
    relation = CatalogItem.where()
    relation = relation.where({status: filters[:status]}) if filters.has_key?(:status)
    result = relation.all
    return problem(503, "storage_unavailable", "catalog storage is unavailable") if result.err?
    items = result.value
    render(json: {
      data: items.take(100).map |item|: item_json(item),
      meta: {count: items.count, truncated: items.count > 100}
    })

  def create():
    attrs = params.json.expect(service(:schemas).create_item)
    now = Time.now.iso8601
    values = storage_attrs(attrs)
    values[:version] = 1
    values[:created_at] = now
    values[:updated_at] = now
    result = CatalogItem.create(values)
    return persistence_problem(result) if result.err?
    item = result.value
    render(
      json: item_json(item),
      status: 201,
      headers: {
        "location": "/v1/items/" + item[:id].to_str,
        "etag": item[:version].to_str
      })

  def show(id as Int):
    result = find_item(id)
    return result.error if result.err?
    item = result.value
    render(json: item_json(item), headers: {"etag": item[:version].to_str})

  def replace(id as Int):
    result = find_item(id)
    return result.error if result.err?
    update_item(
      result.value,
      params.json.expect(service(:schemas).create_item),
      request)

  def patch(id as Int):
    result = find_item(id)
    return result.error if result.err?
    update_item(
      result.value,
      params.json.expect(service(:schemas).patch_item),
      request,
      partial: true)

  def destroy(id as Int):
    result = find_item(id)
    return result.error if result.err?
    item = result.value
    version = requested_version(request)
    return version.error if version.err?
    return problem(409, "version_conflict", "catalog item version is stale") unless version.value == item[:version]
    deleted = item.delete
    return persistence_problem(deleted) if deleted.err?
    render(status: 204)

class QuietSink:
  def emit(kind, fields):
    null

def liveness(request):
  request.service(:telemetry).health.liveness(request)

def readiness(request):
  request.service(:telemetry).health.readiness(request)

def service_routes():
  ember.routes:
    get "/v1/items", to: &Items#index
    post "/v1/items", to: &Items#create
    get "/v1/items/:id", to: &Items#show, params: {id: Int}
    put "/v1/items/:id", to: &Items#replace, params: {id: Int}
    patch "/v1/items/:id", to: &Items#patch, params: {id: Int}
    delete "/v1/items/:id", to: &Items#destroy, params: {id: Int}
    get "/health/live", to: &liveness
    get "/health/ready", to: &readiness

def migrate(database):
  database.with_connection |connection|:
    connection.exec("""
      create table catalog_items(
        id integer primary key autoincrement,
        sku text not null unique,
        name text not null,
        description text not null,
        category text not null,
        status text not null,
        price_cents integer not null,
        currency text not null,
        stock_count integer not null,
        weight_grams integer not null,
        active integer not null,
        manufacturer text not null,
        country_code text not null,
        barcode text not null,
        color text not null,
        size text not null,
        rating_milli integer not null,
        tags_json text not null,
        metadata_json text not null,
        version integer not null,
        created_at text not null,
        updated_at text not null
      )
    """)
    connection.exec("create index catalog_items_status on catalog_items(status)")

def arguments():
  parser = ArgParser()
  parser.name("sputnik-polyglot-http-rps-server")
  parser.arg("--host", default: "127.0.0.1")
  parser.arg("--port", type: Int, default: 3340)
  parser.arg("--workers", type: Int, default: 4)
  parser.arg("--pool-size", type: Int, default: 1)
  parser.parse_or_raise()

def main():
  args = arguments()
  pool_size = args["pool_size"]
  raise ArgumentError("--pool-size must be positive") unless pool_size > 0
  database = memory_pool(
    name: "sputnik_polyglot_http_rps",
    min_size: pool_size,
    max_size: pool_size,
    checkout_timeout: 1.0,
    idle_timeout: 60.0,
    busy_timeout: 2.0,
    pragmas: {foreign_keys: true})
  migrate(database)
  database.bind!(CatalogItem).or_raise
  telemetry = Telemetry(
    sink: QuietSink(),
    metrics: MetricsRegistry(max_series: 256),
    tracer: false)
  app = ember.App(
    routes: service_routes(),
    integrations: [orm_pool(database, name: "catalog")],
    services: {
      telemetry: telemetry,
      schemas: Schemas()
    },
    security: ember.SecurityPolicy(allowed_hosts: ["localhost", "127.0.0.1", "::1"]),
    telemetry: telemetry)
  app.server(
    host: args["host"],
    port: args["port"],
    workers: args["workers"],
    max_concurrent_per_worker: 32,
    backlog: 128,
    max_header_bytes: 32768,
    max_body_bytes: 65536,
    read_timeout: 15.0,
    write_timeout: 15.0,
    idle_timeout: 10.0,
    max_requests_per_connection: 500,
    shutdown_timeout: 30.0).serve()
