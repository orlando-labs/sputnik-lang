# frozen_string_literal: true

# Rails API/Puma counterpart for the Ember request-flow benchmark. This is a
# framework lane, deliberately separate from the raw Ruby polyglot workloads.

require "action_controller/railtie"
require "active_record"
require "json"
require "logger"
require "sqlite3"
require "time"

class PolyglotRailsApp < Rails::Application
  config.api_only = true
  config.eager_load = true
  config.consider_all_requests_local = false
  config.action_dispatch.show_exceptions = :none
  config.hosts = ["127.0.0.1", "localhost", "::1"]
  config.logger = Logger.new(IO::NULL)
  config.log_level = :fatal
  config.secret_key_base = "amber-polyglot-http-rps-benchmark"
end

module CatalogContract
  MAX_BODY_BYTES = 65_536
  REQUIRED_FIELDS = %w[
    sku name description category status price_cents currency stock_count
    weight_grams active manufacturer country_code barcode color size
    rating_milli tags metadata
  ].freeze
  ALLOWED_FIELDS = REQUIRED_FIELDS.to_h { |field| [field, true] }.freeze
  INTEGER_FIELDS = %w[price_cents stock_count weight_grams rating_milli].freeze
  STRING_FIELDS = (
    REQUIRED_FIELDS - INTEGER_FIELDS - %w[active tags metadata]
  ).freeze
  CATEGORIES = %w[hardware software service accessory].freeze
  STATUSES = %w[draft active paused retired].freeze

  module_function

  def schema_error(item, patch: false)
    return "item" unless item.is_a?(Hash)
    return "unknown" unless item.keys.all? { |key| ALLOWED_FIELDS[key] }
    return "missing" unless patch || REQUIRED_FIELDS.all? { |field| item.key?(field) }
    return "string" unless STRING_FIELDS.all? do |field|
      !item.key?(field) || item[field].is_a?(String)
    end
    return "integer" unless INTEGER_FIELDS.all? do |field|
      !item.key?(field) || item[field].is_a?(Integer)
    end
    return "active" if item.key?("active") && ![true, false].include?(item["active"])
    if item.key?("tags")
      tags = item["tags"]
      return "tags" unless tags.is_a?(Array) && tags.all? { |tag| tag.is_a?(String) }
    end
    if item.key?("metadata")
      metadata = item["metadata"]
      return "metadata" unless metadata.is_a?(Hash)
      return "metadata" unless metadata.keys.sort == %w[batch fragile source]
      return "metadata" unless metadata["source"].is_a?(String)
      return "metadata" unless metadata["batch"].is_a?(String)
      return "metadata" unless [true, false].include?(metadata["fragile"])
    end
    nil
  end

  def bounded?(value, minimum, maximum)
    value.length.between?(minimum, maximum)
  end

  def record_error(record)
    sku = record[:sku]
    return "sku" unless sku.is_a?(String) && bounded?(sku, 3, 64) && /\A[A-Z0-9_-]+\z/.match?(sku)
    return "name" unless record[:name].is_a?(String) && bounded?(record[:name], 1, 160)
    return "description" unless record[:description].is_a?(String) && bounded?(record[:description], 1, 2000)
    return "category" unless CATEGORIES.include?(record[:category])
    return "status" unless STATUSES.include?(record[:status])
    return "price_cents" unless record[:price_cents].is_a?(Integer) && record[:price_cents].between?(0, 1_000_000_000)
    return "currency" unless record[:currency].is_a?(String) && /\A[A-Z]{3}\z/.match?(record[:currency])
    return "stock_count" unless record[:stock_count].is_a?(Integer) && record[:stock_count].between?(0, 1_000_000)
    return "weight_grams" unless record[:weight_grams].is_a?(Integer) && record[:weight_grams].between?(1, 10_000_000)
    return "active" unless [0, 1].include?(record[:active])
    return "manufacturer" unless record[:manufacturer].is_a?(String) && bounded?(record[:manufacturer], 1, 120)
    return "country_code" unless record[:country_code].is_a?(String) && /\A[A-Z]{2}\z/.match?(record[:country_code])
    barcode = record[:barcode]
    return "barcode" unless barcode.is_a?(String) && bounded?(barcode, 8, 32) && /\A[0-9]+\z/.match?(barcode)
    return "color" unless record[:color].is_a?(String) && bounded?(record[:color], 1, 40)
    return "size" unless record[:size].is_a?(String) && bounded?(record[:size], 1, 40)
    return "rating_milli" unless record[:rating_milli].is_a?(Integer) && record[:rating_milli].between?(0, 5000)

    tags = JSON.parse(record[:tags_json])
    return "tags" unless tags.is_a?(Array) && tags.length <= 16 && tags.all? { |tag| tag.is_a?(String) && bounded?(tag, 1, 32) }
    metadata = JSON.parse(record[:metadata_json])
    return "metadata" unless metadata.is_a?(Hash)
    return "metadata" unless metadata.keys.sort == %w[batch fragile source]
    return "metadata" unless metadata["source"].is_a?(String) && bounded?(metadata["source"], 1, 64)
    return "metadata" unless metadata["batch"].is_a?(String) && bounded?(metadata["batch"], 1, 64)
    return "metadata" unless [true, false].include?(metadata["fragile"])
    nil
  rescue JSON::ParserError, TypeError
    "tags"
  end
end

module CatalogDatabase
  URI = "file:amber_polyglot_rails?mode=memory&cache=shared"
  # Keep lock contention from changing the shared HTTP contract into a
  # driver-specific retry-policy comparison.
  LOCK = Mutex.new

  module_function

  def prepare!
    ActiveRecord::Base.establish_connection(
      adapter: "sqlite3",
      database: URI,
      uri: true,
      pool: 4,
      timeout: 2_000
    )
    ActiveRecord::Base.logger = Logger.new(IO::NULL)
    connection = ActiveRecord::Base.connection
    connection.execute("PRAGMA foreign_keys = ON")
    connection.execute("PRAGMA busy_timeout = 2000")
    connection.execute(<<~SQL)
      CREATE TABLE catalog_items(
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
    SQL
    connection.execute("CREATE INDEX catalog_items_status ON catalog_items(status)")
  end

  def synchronize(&block)
    LOCK.synchronize(&block)
  end
end

class CatalogItem < ActiveRecord::Base
  self.table_name = "catalog_items"

  validates :sku, uniqueness: true
  validate :catalog_contract

  private

  def catalog_contract
    issue = CatalogContract.record_error(self)
    errors.add(issue, :invalid) if issue
  end
end

class ItemsController < ActionController::API
  STORED_ATTRIBUTES = %w[
    sku name description category status price_cents currency stock_count
    weight_grams manufacturer country_code barcode color size rating_milli
  ].freeze
  METADATA_FIELDS = %w[source batch fragile].freeze
  PARAM_FILTER = [
    :sku, :name, :description, :category, :status, :price_cents, :currency,
    :stock_count, :weight_grams, :active, :manufacturer, :country_code,
    :barcode, :color, :size, :rating_milli,
    { tags: [], metadata: %i[source batch fragile] }
  ].freeze

  before_action :require_integer_id, only: %i[show update patch destroy]
  around_action :with_catalog_lock

  def ready
    render json: { status: "ready" }
  end

  def index
    unknown = request.query_parameters.keys - ["status"]
    return problem(400, "invalid_parameters") unless unknown.empty?

    status = params.permit(:status)[:status]
    return validation_problem("status") if status && !CatalogContract::STATUSES.include?(status)

    relation = CatalogItem.all
    relation = relation.where(status: status) if status
    items = relation.to_a
    render json: {
      data: items.take(100).map { |item| item_json(item) },
      meta: { count: items.length, truncated: items.length > 100 }
    }
  rescue ActiveRecord::StatementInvalid
    storage_problem
  end

  def show
    item = CatalogItem.find_by(id: @item_id)
    return problem(404, "not_found") unless item

    response.set_header("ETag", item[:version].to_s)
    render json: item_json(item)
  rescue ActiveRecord::StatementInvalid
    storage_problem
  end

  def create
    attrs = parsed_item
    return if performed?

    now = Time.now.utc.iso8601(6)
    item = CatalogItem.new(storage_attributes(attrs).merge(
      "version" => 1,
      "created_at" => now,
      "updated_at" => now
    ))
    return unless save_item(item)

    response.set_header("Location", "/v1/items/#{item[:id]}")
    response.set_header("ETag", item[:version].to_s)
    render json: item_json(item), status: 201
  end

  def update
    mutate(patch: false)
  end

  def patch
    mutate(patch: true)
  end

  def destroy
    item = CatalogItem.find_by(id: @item_id)
    return problem(404, "not_found") unless item

    version = requested_version
    return if performed?
    return problem(409, "version_conflict") unless item[:version] == version

    item.delete
    head :no_content
  rescue ActiveRecord::StatementInvalid
    storage_problem
  end

  def method_not_allowed
    problem(405, "method_not_allowed")
  end

  private

  def with_catalog_lock(&block)
    CatalogDatabase.synchronize(&block)
  end

  def require_integer_id
    text = params[:id].to_s
    return problem(400, "invalid_path_parameter") unless /\A[0-9]+\z/.match?(text)

    @item_id = text.to_i
  end

  def parsed_item(patch: false)
    length = request.content_length.to_i
    return problem(413, "payload_too_large") if length > CatalogContract::MAX_BODY_BYTES
    return problem(415, "unsupported_media_type") unless request.media_type == "application/json"

    begin
      document = params
      raw_item = document[:item]
      return problem(400, "invalid_document") unless raw_item.is_a?(ActionController::Parameters)
      return problem(400, "invalid_parameters") unless raw_item.keys.all? { |key| CatalogContract::ALLOWED_FIELDS[key] }
      raw_metadata = raw_item[:metadata]
      if raw_metadata.is_a?(ActionController::Parameters)
        return problem(400, "invalid_parameters") unless raw_metadata.keys.all? { |key| METADATA_FIELDS.include?(key) }
      end
      item = if patch
        raw_item.permit(*PARAM_FILTER).to_h
      else
        document.expect(item: PARAM_FILTER).to_h
      end
    rescue ActionDispatch::Http::Parameters::ParseError
      return problem(400, "invalid_json")
    rescue ActionController::ParameterMissing, ActionController::UnpermittedParameters
      return problem(400, "invalid_parameters")
    end
    error = CatalogContract.schema_error(item, patch: patch)
    return problem(400, "invalid_parameters") if error

    item
  end

  def requested_version
    text = request.headers["HTTP_IF_MATCH"]
    return problem(428, "precondition_required") unless text
    return problem(400, "invalid_precondition") unless /\A[0-9]{1,18}\z/.match?(text)

    version = text.to_i
    return problem(400, "invalid_precondition") unless version.positive?

    version
  end

  def mutate(patch:)
    item = CatalogItem.find_by(id: @item_id)
    return problem(404, "not_found") unless item

    version = requested_version
    return if performed?

    attrs = parsed_item(patch: patch)
    return if performed?
    return validation_problem("item") if patch && attrs.empty?
    return problem(409, "version_conflict") unless item[:version] == version

    item.assign_attributes(storage_attributes(attrs))
    item[:version] += 1
    item[:updated_at] = Time.now.utc.iso8601(6)
    return unless save_item(item)

    response.set_header("ETag", item[:version].to_s)
    render json: item_json(item)
  rescue ActiveRecord::StatementInvalid
    storage_problem
  end

  def storage_attributes(attrs)
    values = attrs.slice(*STORED_ATTRIBUTES)
    values["active"] = attrs["active"] ? 1 : 0 if attrs.key?("active")
    values["tags_json"] = JSON.generate(attrs["tags"]) if attrs.key?("tags")
    values["metadata_json"] = JSON.generate(attrs["metadata"]) if attrs.key?("metadata")
    values
  end

  def item_json(item)
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
      tags: JSON.parse(item[:tags_json]),
      metadata: JSON.parse(item[:metadata_json]),
      version: item[:version],
      created_at: item[:created_at],
      updated_at: item[:updated_at]
    }
  end

  def save_item(item)
    return true if item.save

    return conflict_problem if item.errors.of_kind?(:sku, :taken)

    validation_problem(item.errors.attribute_names.first.to_s)
    false
  rescue ActiveRecord::RecordNotUnique
    conflict_problem
  rescue ActiveRecord::StatementInvalid
    storage_problem
  end

  def conflict_problem
    problem(409, "sku_conflict")
    false
  end

  def storage_problem
    problem(503, "storage_unavailable")
    false
  end

  def validation_problem(field)
    problem(
      422,
      "validation_failed",
      details: [{ field: field, code: "invalid" }]
    )
  end

  def problem(status, code, details: [])
    render(
      json: {
        error: {
          code: code,
          message: code.tr("_", " "),
          details: details
        }
      },
      status: status
    )
    nil
  end
end

PolyglotRailsApp.initialize!
CatalogDatabase.prepare!

PolyglotRailsApp.routes.draw do
  get "/health/ready", to: "items#ready"
  get "/health/live", to: "items#ready"
  get "/v1/items", to: "items#index"
  post "/v1/items", to: "items#create"
  get "/v1/items/:id", to: "items#show"
  put "/v1/items/:id", to: "items#update"
  patch "/v1/items/:id", to: "items#patch"
  delete "/v1/items/:id", to: "items#destroy"
  match "/v1/items", to: "items#method_not_allowed", via: :all
  match "/v1/items/:id", to: "items#method_not_allowed", via: :all
end
