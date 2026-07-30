# frozen_string_literal: true

# Rails API/Puma counterpart for the Ember request-flow benchmark. This is a
# framework lane, deliberately separate from the raw Ruby polyglot workloads.

require "action_controller/railtie"
require "json"
require "logger"
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

  def deep_copy(value)
    case value
    when Hash
      value.to_h { |key, nested| [key, deep_copy(nested)] }
    when Array
      value.map { |nested| deep_copy(nested) }
    else
      value
    end
  end

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

  def model_error(item)
    sku = item["sku"]
    return "sku" unless bounded?(sku, 3, 64) && /\A[A-Z0-9_-]+\z/.match?(sku)
    return "name" unless bounded?(item["name"], 1, 160)
    return "description" unless bounded?(item["description"], 1, 2000)
    return "category" unless CATEGORIES.include?(item["category"])
    return "status" unless STATUSES.include?(item["status"])
    return "price_cents" unless item["price_cents"].between?(0, 1_000_000_000)
    return "currency" unless /\A[A-Z]{3}\z/.match?(item["currency"])
    return "stock_count" unless item["stock_count"].between?(0, 1_000_000)
    return "weight_grams" unless item["weight_grams"].between?(1, 10_000_000)
    return "manufacturer" unless bounded?(item["manufacturer"], 1, 120)
    return "country_code" unless /\A[A-Z]{2}\z/.match?(item["country_code"])
    barcode = item["barcode"]
    return "barcode" unless bounded?(barcode, 8, 32) && /\A[0-9]+\z/.match?(barcode)
    return "color" unless bounded?(item["color"], 1, 40)
    return "size" unless bounded?(item["size"], 1, 40)
    return "rating_milli" unless item["rating_milli"].between?(0, 5000)
    tags = item["tags"]
    return "tags" unless tags.length <= 16 && tags.all? { |tag| bounded?(tag, 1, 32) }
    metadata = item["metadata"]
    return "metadata" unless bounded?(metadata["source"], 1, 64)
    return "metadata" unless bounded?(metadata["batch"], 1, 64)
    nil
  end
end

class CatalogStore
  def initialize
    @mutex = Mutex.new
    @next_id = 1
    @items = {}
    @sku_ids = {}
  end

  def create(attrs)
    @mutex.synchronize do
      return { kind: :conflict } if @sku_ids.key?(attrs["sku"])

      now = Time.now.utc.iso8601(6)
      item = CatalogContract.deep_copy(attrs)
      item.merge!(
        "id" => @next_id,
        "version" => 1,
        "created_at" => now,
        "updated_at" => now
      )
      @next_id += 1
      @items[item["id"]] = item
      @sku_ids[item["sku"]] = item["id"]
      { kind: :ok, item: CatalogContract.deep_copy(item) }
    end
  end

  def find(id)
    @mutex.synchronize do
      item = @items[id]
      item && CatalogContract.deep_copy(item)
    end
  end

  def list(status)
    @mutex.synchronize do
      @items.values.filter_map do |item|
        CatalogContract.deep_copy(item) if status.nil? || item["status"] == status
      end
    end
  end

  def update(id, attrs, version)
    @mutex.synchronize do
      item = @items[id]
      return { kind: :not_found } unless item
      return { kind: :version_conflict } unless item["version"] == version

      candidate = CatalogContract.deep_copy(item).merge(CatalogContract.deep_copy(attrs))
      issue = CatalogContract.model_error(candidate)
      return { kind: :invalid, issue: issue } if issue

      owner = @sku_ids[candidate["sku"]]
      return { kind: :conflict } if owner && owner != id

      if candidate["sku"] != item["sku"]
        @sku_ids.delete(item["sku"])
        @sku_ids[candidate["sku"]] = id
      end
      candidate["version"] = item["version"] + 1
      candidate["updated_at"] = Time.now.utc.iso8601(6)
      @items[id] = candidate
      { kind: :ok, item: CatalogContract.deep_copy(candidate) }
    end
  end

  def delete(id, version)
    @mutex.synchronize do
      item = @items[id]
      return { kind: :not_found } unless item
      return { kind: :version_conflict } unless item["version"] == version

      @items.delete(id)
      @sku_ids.delete(item["sku"])
      { kind: :ok }
    end
  end
end

class ItemsController < ActionController::API
  STORE = CatalogStore.new

  before_action :require_integer_id, only: %i[show update patch destroy]

  def ready
    render json: { status: "ready" }
  end

  def index
    unknown = request.query_parameters.keys - ["status"]
    return problem(400, "invalid_parameters") unless unknown.empty?

    status = request.query_parameters["status"]
    return validation_problem("status") if status && !CatalogContract::STATUSES.include?(status)

    items = STORE.list(status)
    render json: {
      data: items.take(100),
      meta: { count: items.length, truncated: items.length > 100 }
    }
  end

  def show
    item = STORE.find(@item_id)
    return problem(404, "not_found") unless item

    response.set_header("ETag", item["version"].to_s)
    render json: item
  end

  def create
    item = parsed_item
    return if performed?

    issue = CatalogContract.model_error(item)
    return validation_problem(issue) if issue

    result = STORE.create(item)
    return store_problem(result) unless result[:kind] == :ok

    created = result[:item]
    response.set_header("Location", "/v1/items/#{created['id']}")
    response.set_header("ETag", created["version"].to_s)
    render json: created, status: 201
  end

  def update
    mutate(patch: false)
  end

  def patch
    mutate(patch: true)
  end

  def destroy
    return problem(404, "not_found") unless STORE.find(@item_id)

    version = requested_version
    return if performed?

    result = STORE.delete(@item_id, version)
    return store_problem(result) unless result[:kind] == :ok

    head :no_content
  end

  def method_not_allowed
    problem(405, "method_not_allowed")
  end

  private

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
      document = JSON.parse(request.raw_post)
    rescue JSON::ParserError
      return problem(400, "invalid_json")
    end
    return problem(400, "invalid_document") unless document.is_a?(Hash)

    item = document["item"]
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
    return problem(404, "not_found") unless STORE.find(@item_id)

    version = requested_version
    return if performed?

    attrs = parsed_item(patch: patch)
    return if performed?
    return validation_problem("item") if patch && attrs.empty?

    result = STORE.update(@item_id, attrs, version)
    return store_problem(result) unless result[:kind] == :ok

    item = result[:item]
    response.set_header("ETag", item["version"].to_s)
    render json: item
  end

  def store_problem(result)
    case result[:kind]
    when :not_found
      problem(404, "not_found")
    when :version_conflict
      problem(409, "version_conflict")
    when :conflict
      problem(409, "sku_conflict")
    when :invalid
      validation_problem(result[:issue])
    else
      problem(500, "internal_error")
    end
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
