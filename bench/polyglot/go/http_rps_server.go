package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

const maxBodyBytes = 65536

var requiredFields = []string{
	"sku", "name", "description", "category", "status", "price_cents",
	"currency", "stock_count", "weight_grams", "active", "manufacturer",
	"country_code", "barcode", "color", "size", "rating_milli", "tags", "metadata",
}

var allowedFields = func() map[string]bool {
	out := make(map[string]bool, len(requiredFields))
	for _, field := range requiredFields {
		out[field] = true
	}
	return out
}()

type item = map[string]any

type storeState struct {
	sync.Mutex
	nextID int64
	items  map[int64]item
	skus   map[string]int64
}

var store = storeState{nextID: 1, items: map[int64]item{}, skus: map[string]int64{}}

func clone(value any) any {
	switch current := value.(type) {
	case map[string]any:
		out := make(map[string]any, len(current))
		for key, nested := range current {
			out[key] = clone(nested)
		}
		return out
	case []any:
		out := make([]any, len(current))
		for index, nested := range current {
			out[index] = clone(nested)
		}
		return out
	default:
		return current
	}
}

func cloneItem(value item) item { return clone(value).(map[string]any) }

func integer(value any) (int64, bool) {
	number, ok := value.(json.Number)
	if !ok {
		return 0, false
	}
	parsed, err := number.Int64()
	return parsed, err == nil
}

func schemaError(value item, patch bool) bool {
	if patch && len(value) == 0 {
		return false
	}
	for key := range value {
		if !allowedFields[key] {
			return true
		}
	}
	if !patch {
		for _, field := range requiredFields {
			if _, ok := value[field]; !ok {
				return true
			}
		}
	}
	stringFields := []string{"sku", "name", "description", "category", "status", "currency", "manufacturer", "country_code", "barcode", "color", "size"}
	for _, field := range stringFields {
		if raw, ok := value[field]; ok {
			if _, valid := raw.(string); !valid {
				return true
			}
		}
	}
	for _, field := range []string{"price_cents", "stock_count", "weight_grams", "rating_milli"} {
		if raw, ok := value[field]; ok {
			if _, valid := integer(raw); !valid {
				return true
			}
		}
	}
	if raw, ok := value["active"]; ok {
		if _, valid := raw.(bool); !valid {
			return true
		}
	}
	if raw, ok := value["tags"]; ok {
		tags, valid := raw.([]any)
		if !valid {
			return true
		}
		for _, rawTag := range tags {
			if _, valid := rawTag.(string); !valid {
				return true
			}
		}
	}
	if raw, ok := value["metadata"]; ok {
		metadata, valid := raw.(map[string]any)
		if !valid || len(metadata) != 3 {
			return true
		}
		if _, valid := metadata["source"].(string); !valid {
			return true
		}
		if _, valid := metadata["batch"].(string); !valid {
			return true
		}
		if _, valid := metadata["fragile"].(bool); !valid {
			return true
		}
	}
	return false
}

func bounded(text string, minimum, maximum int) bool {
	return len(text) >= minimum && len(text) <= maximum
}

func allASCIIIn(text, allowed string) bool {
	for _, char := range []byte(text) {
		if !strings.ContainsRune(allowed, rune(char)) {
			return false
		}
	}
	return true
}

func modelError(value item) string {
	sku := value["sku"].(string)
	if !bounded(sku, 3, 64) || !allASCIIIn(sku, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") {
		return "sku"
	}
	if !bounded(value["name"].(string), 1, 160) {
		return "name"
	}
	if !bounded(value["description"].(string), 1, 2000) {
		return "description"
	}
	if !map[string]bool{"hardware": true, "software": true, "service": true, "accessory": true}[value["category"].(string)] {
		return "category"
	}
	if !map[string]bool{"draft": true, "active": true, "paused": true, "retired": true}[value["status"].(string)] {
		return "status"
	}
	price, _ := integer(value["price_cents"])
	if price < 0 || price > 1_000_000_000 {
		return "price_cents"
	}
	currency := value["currency"].(string)
	if len(currency) != 3 || !allASCIIIn(currency, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") {
		return "currency"
	}
	stock, _ := integer(value["stock_count"])
	if stock < 0 || stock > 1_000_000 {
		return "stock_count"
	}
	weight, _ := integer(value["weight_grams"])
	if weight < 1 || weight > 10_000_000 {
		return "weight_grams"
	}
	if !bounded(value["manufacturer"].(string), 1, 120) {
		return "manufacturer"
	}
	country := value["country_code"].(string)
	if len(country) != 2 || !allASCIIIn(country, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") {
		return "country_code"
	}
	barcode := value["barcode"].(string)
	if !bounded(barcode, 8, 32) || !allASCIIIn(barcode, "0123456789") {
		return "barcode"
	}
	if !bounded(value["color"].(string), 1, 40) {
		return "color"
	}
	if !bounded(value["size"].(string), 1, 40) {
		return "size"
	}
	rating, _ := integer(value["rating_milli"])
	if rating < 0 || rating > 5000 {
		return "rating_milli"
	}
	tags := value["tags"].([]any)
	if len(tags) > 16 {
		return "tags"
	}
	for _, raw := range tags {
		if !bounded(raw.(string), 1, 32) {
			return "tags"
		}
	}
	metadata := value["metadata"].(map[string]any)
	if !bounded(metadata["source"].(string), 1, 64) || !bounded(metadata["batch"].(string), 1, 64) {
		return "metadata"
	}
	return ""
}

func payload(w http.ResponseWriter, status int, value any) {
	var body []byte
	if value != nil {
		body, _ = json.Marshal(value)
		w.Header().Set("Content-Type", "application/json")
	}
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(status)
	if len(body) != 0 {
		_, _ = w.Write(body)
	}
}

func problem(w http.ResponseWriter, status int, code, field string) {
	details := []any{}
	if field != "" {
		details = append(details, map[string]any{"field": field, "code": "invalid"})
	}
	payload(w, status, map[string]any{"error": map[string]any{"code": code, "message": code, "details": details}})
}

func authorized(r *http.Request) bool {
	return strings.HasPrefix(r.Host, "127.0.0.1") || strings.HasPrefix(r.Host, "localhost") || strings.HasPrefix(r.Host, "[::1]")
}

func readItem(w http.ResponseWriter, r *http.Request, patch bool) (item, bool) {
	if !strings.HasPrefix(strings.ToLower(r.Header.Get("Content-Type")), "application/json") {
		problem(w, 415, "request_error", "")
		return nil, false
	}
	if r.ContentLength > maxBodyBytes {
		problem(w, 413, "request_error", "")
		return nil, false
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, maxBodyBytes+1))
	if err != nil || len(body) > maxBodyBytes {
		problem(w, 413, "request_error", "")
		return nil, false
	}
	decoder := json.NewDecoder(strings.NewReader(string(body)))
	decoder.UseNumber()
	var document any
	if decoder.Decode(&document) != nil {
		problem(w, 400, "request_error", "")
		return nil, false
	}
	root, ok := document.(map[string]any)
	if !ok {
		problem(w, 400, "request_error", "")
		return nil, false
	}
	value, ok := root["item"].(map[string]any)
	if !ok || schemaError(value, patch) {
		problem(w, 400, "request_error", "")
		return nil, false
	}
	return value, true
}

func parseMember(path string) (int64, bool) {
	const prefix = "/v1/items/"
	if !strings.HasPrefix(path, prefix) {
		return 0, false
	}
	id, err := strconv.ParseInt(strings.TrimPrefix(path, prefix), 10, 64)
	return id, err == nil
}

func ifMatch(w http.ResponseWriter, r *http.Request, version int64) bool {
	text := r.Header.Get("If-Match")
	if text == "" {
		problem(w, 428, "request_error", "")
		return false
	}
	value, err := strconv.ParseInt(text, 10, 64)
	if err != nil {
		problem(w, 400, "request_error", "")
		return false
	}
	if value != version {
		problem(w, 409, "request_error", "")
		return false
	}
	return true
}

func create(w http.ResponseWriter, r *http.Request) {
	value, ok := readItem(w, r, false)
	if !ok {
		return
	}
	if field := modelError(value); field != "" {
		problem(w, 422, "validation_failed", field)
		return
	}
	store.Lock()
	defer store.Unlock()
	sku := value["sku"].(string)
	if _, exists := store.skus[sku]; exists {
		problem(w, 409, "request_error", "")
		return
	}
	id := store.nextID
	store.nextID++
	record := cloneItem(value)
	now := strconv.FormatInt(time.Now().UnixNano(), 10)
	record["id"] = id
	record["version"] = int64(1)
	record["created_at"] = now
	record["updated_at"] = now
	store.items[id] = record
	store.skus[sku] = id
	payload(w, 201, record)
}

func listItems(w http.ResponseWriter, r *http.Request) {
	for key := range r.URL.Query() {
		if key != "status" {
			problem(w, 400, "request_error", "")
			return
		}
	}
	status := r.URL.Query().Get("status")
	if status != "" && !map[string]bool{"draft": true, "active": true, "paused": true, "retired": true}[status] {
		problem(w, 422, "validation_failed", "status")
		return
	}
	store.Lock()
	values := make([]any, 0, len(store.items))
	for _, record := range store.items {
		if status == "" || record["status"] == status {
			values = append(values, cloneItem(record))
		}
	}
	store.Unlock()
	payload(w, 200, map[string]any{"data": values})
}

func show(w http.ResponseWriter, id int64) {
	store.Lock()
	record, ok := store.items[id]
	var result item
	if ok {
		result = cloneItem(record)
	}
	store.Unlock()
	if !ok {
		problem(w, 404, "request_error", "")
		return
	}
	payload(w, 200, result)
}

func mutate(w http.ResponseWriter, r *http.Request, id int64, patch bool) {
	store.Lock()
	current, exists := store.items[id]
	var snapshot item
	if exists {
		snapshot = cloneItem(current)
	}
	store.Unlock()
	if !exists {
		problem(w, 404, "request_error", "")
		return
	}
	version, _ := snapshot["version"].(int64)
	if !ifMatch(w, r, version) {
		return
	}
	incoming, ok := readItem(w, r, patch)
	if !ok {
		return
	}
	if patch && len(incoming) == 0 {
		problem(w, 422, "validation_failed", "")
		return
	}
	var candidate item
	if patch {
		candidate = snapshot
		for key, value := range incoming {
			candidate[key] = clone(value)
		}
	} else {
		candidate = incoming
	}
	if field := modelError(candidate); field != "" {
		problem(w, 422, "validation_failed", field)
		return
	}
	store.Lock()
	defer store.Unlock()
	current, exists = store.items[id]
	if !exists {
		problem(w, 404, "request_error", "")
		return
	}
	currentVersion, _ := current["version"].(int64)
	if currentVersion != version {
		problem(w, 409, "request_error", "")
		return
	}
	newSKU := candidate["sku"].(string)
	if owner, found := store.skus[newSKU]; found && owner != id {
		problem(w, 409, "request_error", "")
		return
	}
	record := make(item, len(requiredFields)+4)
	for _, field := range requiredFields {
		record[field] = clone(candidate[field])
	}
	oldSKU := current["sku"].(string)
	record["id"] = id
	record["version"] = version + 1
	record["created_at"] = current["created_at"]
	record["updated_at"] = strconv.FormatInt(time.Now().UnixNano(), 10)
	if oldSKU != newSKU {
		delete(store.skus, oldSKU)
		store.skus[newSKU] = id
	}
	store.items[id] = record
	payload(w, 200, record)
}

func deleteItem(w http.ResponseWriter, r *http.Request, id int64) {
	store.Lock()
	current, exists := store.items[id]
	var snapshot item
	if exists {
		snapshot = cloneItem(current)
	}
	store.Unlock()
	if !exists {
		problem(w, 404, "request_error", "")
		return
	}
	version, _ := snapshot["version"].(int64)
	if !ifMatch(w, r, version) {
		return
	}
	store.Lock()
	current, exists = store.items[id]
	if exists {
		delete(store.items, id)
		delete(store.skus, current["sku"].(string))
	}
	store.Unlock()
	if !exists {
		problem(w, 404, "request_error", "")
		return
	}
	payload(w, 204, nil)
}

func handler(w http.ResponseWriter, r *http.Request) {
	if !authorized(r) {
		problem(w, 403, "request_error", "")
		return
	}
	if r.URL.Path == "/health/ready" && r.Method == http.MethodGet {
		payload(w, 200, map[string]any{"status": "ready"})
		return
	}
	if r.URL.Path == "/v1/items" {
		switch r.Method {
		case http.MethodGet:
			listItems(w, r)
		case http.MethodPost:
			create(w, r)
		default:
			problem(w, 405, "request_error", "")
		}
		return
	}
	id, valid := parseMember(r.URL.Path)
	if strings.HasPrefix(r.URL.Path, "/v1/items/") && !valid {
		problem(w, 400, "request_error", "")
		return
	}
	if !valid {
		problem(w, 404, "request_error", "")
		return
	}
	switch r.Method {
	case http.MethodGet:
		show(w, id)
	case http.MethodPut:
		mutate(w, r, id, false)
	case http.MethodPatch:
		mutate(w, r, id, true)
	case http.MethodDelete:
		deleteItem(w, r, id)
	default:
		problem(w, 405, "request_error", "")
	}
}

func main() {
	host := flag.String("host", "127.0.0.1", "listen host")
	port := flag.Int("port", 3400, "listen port")
	flag.Parse()
	server := &http.Server{
		Addr:              fmt.Sprintf("%s:%d", *host, *port),
		Handler:           http.HandlerFunc(handler),
		ReadHeaderTimeout: 15 * time.Second,
		IdleTimeout:       30 * time.Second,
	}
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		panic(err)
	}
}
