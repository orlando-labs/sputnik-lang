use std::collections::HashMap;
use std::env;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::str;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Clone, Debug)]
enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Str(String),
    Array(Vec<Json>),
    Object(HashMap<String, Json>),
}

struct JsonParser<'a> {
    input: &'a [u8],
    offset: usize,
}

impl<'a> JsonParser<'a> {
    fn new(input: &'a [u8]) -> Self {
        Self { input, offset: 0 }
    }

    fn parse(mut self) -> Result<Json, ()> {
        let value = self.value()?;
        self.space();
        if self.offset == self.input.len() { Ok(value) } else { Err(()) }
    }

    fn space(&mut self) {
        while self.offset < self.input.len() && self.input[self.offset].is_ascii_whitespace() {
            self.offset += 1;
        }
    }

    fn take(&mut self, expected: u8) -> Result<(), ()> {
        if self.input.get(self.offset) == Some(&expected) {
            self.offset += 1;
            Ok(())
        } else {
            Err(())
        }
    }

    fn literal(&mut self, text: &[u8], value: Json) -> Result<Json, ()> {
        if self.input.get(self.offset..self.offset + text.len()) == Some(text) {
            self.offset += text.len();
            Ok(value)
        } else {
            Err(())
        }
    }

    fn value(&mut self) -> Result<Json, ()> {
        self.space();
        match self.input.get(self.offset).copied() {
            Some(b'n') => self.literal(b"null", Json::Null),
            Some(b't') => self.literal(b"true", Json::Bool(true)),
            Some(b'f') => self.literal(b"false", Json::Bool(false)),
            Some(b'"') => self.string().map(Json::Str),
            Some(b'[') => self.array(),
            Some(b'{') => self.object(),
            Some(b'-' | b'0'..=b'9') => self.integer(),
            _ => Err(()),
        }
    }

    fn string(&mut self) -> Result<String, ()> {
        self.take(b'"')?;
        let mut result = String::new();
        while let Some(byte) = self.input.get(self.offset).copied() {
            self.offset += 1;
            match byte {
                b'"' => return Ok(result),
                b'\\' => {
                    let escaped = *self.input.get(self.offset).ok_or(())?;
                    self.offset += 1;
                    match escaped {
                        b'"' => result.push('"'),
                        b'\\' => result.push('\\'),
                        b'/' => result.push('/'),
                        b'b' => result.push('\u{0008}'),
                        b'f' => result.push('\u{000c}'),
                        b'n' => result.push('\n'),
                        b'r' => result.push('\r'),
                        b't' => result.push('\t'),
                        b'u' => {
                            let digits = self.input.get(self.offset..self.offset + 4).ok_or(())?;
                            let text = str::from_utf8(digits).map_err(|_| ())?;
                            let code = u32::from_str_radix(text, 16).map_err(|_| ())?;
                            result.push(char::from_u32(code).ok_or(())?);
                            self.offset += 4;
                        }
                        _ => return Err(()),
                    }
                }
                0..=31 => return Err(()),
                32..=127 => result.push(byte as char),
                _ => {
                    let width = if byte & 0xe0 == 0xc0 { 2 } else if byte & 0xf0 == 0xe0 { 3 } else if byte & 0xf8 == 0xf0 { 4 } else { return Err(()); };
                    let start = self.offset - 1;
                    let text = str::from_utf8(self.input.get(start..start + width).ok_or(())?).map_err(|_| ())?;
                    result.push_str(text);
                    self.offset = start + width;
                }
            }
        }
        Err(())
    }

    fn integer(&mut self) -> Result<Json, ()> {
        let start = self.offset;
        if self.input.get(self.offset) == Some(&b'-') { self.offset += 1; }
        let digits = self.offset;
        while matches!(self.input.get(self.offset), Some(b'0'..=b'9')) { self.offset += 1; }
        if self.offset == digits || matches!(self.input.get(self.offset), Some(b'.' | b'e' | b'E')) { return Err(()); }
        let text = str::from_utf8(&self.input[start..self.offset]).map_err(|_| ())?;
        Ok(Json::Int(text.parse().map_err(|_| ())?))
    }

    fn array(&mut self) -> Result<Json, ()> {
        self.take(b'[')?;
        self.space();
        let mut values = Vec::new();
        if self.input.get(self.offset) == Some(&b']') { self.offset += 1; return Ok(Json::Array(values)); }
        loop {
            values.push(self.value()?);
            self.space();
            match self.input.get(self.offset) {
                Some(b',') => self.offset += 1,
                Some(b']') => { self.offset += 1; break; }
                _ => return Err(()),
            }
        }
        Ok(Json::Array(values))
    }

    fn object(&mut self) -> Result<Json, ()> {
        self.take(b'{')?;
        self.space();
        let mut values = HashMap::new();
        if self.input.get(self.offset) == Some(&b'}') { self.offset += 1; return Ok(Json::Object(values)); }
        loop {
            self.space();
            let key = self.string()?;
            self.space();
            self.take(b':')?;
            let value = self.value()?;
            values.insert(key, value);
            self.space();
            match self.input.get(self.offset) {
                Some(b',') => self.offset += 1,
                Some(b'}') => { self.offset += 1; break; }
                _ => return Err(()),
            }
        }
        Ok(Json::Object(values))
    }
}

fn escape(text: &str, out: &mut String) {
    out.push('"');
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            ch if ch < ' ' => out.push_str(&format!("\\u{:04x}", ch as u32)),
            ch => out.push(ch),
        }
    }
    out.push('"');
}

fn encode(value: &Json, out: &mut String) {
    match value {
        Json::Null => out.push_str("null"),
        Json::Bool(value) => out.push_str(if *value { "true" } else { "false" }),
        Json::Int(value) => out.push_str(&value.to_string()),
        Json::Str(value) => escape(value, out),
        Json::Array(values) => {
            out.push('[');
            for (index, value) in values.iter().enumerate() {
                if index != 0 { out.push(','); }
                encode(value, out);
            }
            out.push(']');
        }
        Json::Object(values) => {
            out.push('{');
            for (index, (key, value)) in values.iter().enumerate() {
                if index != 0 { out.push(','); }
                escape(key, out);
                out.push(':');
                encode(value, out);
            }
            out.push('}');
        }
    }
}

fn object(entries: impl IntoIterator<Item = (&'static str, Json)>) -> Json {
    Json::Object(entries.into_iter().map(|(key, value)| (key.to_string(), value)).collect())
}

const REQUIRED: &[&str] = &[
    "sku", "name", "description", "category", "status", "price_cents", "currency",
    "stock_count", "weight_grams", "active", "manufacturer", "country_code", "barcode",
    "color", "size", "rating_milli", "tags", "metadata",
];

fn text<'a>(item: &'a HashMap<String, Json>, key: &str) -> Option<&'a str> {
    match item.get(key) { Some(Json::Str(value)) => Some(value), _ => None }
}
fn integer(item: &HashMap<String, Json>, key: &str) -> Option<i64> {
    match item.get(key) { Some(Json::Int(value)) => Some(*value), _ => None }
}

fn schema_error(item: &HashMap<String, Json>, patch: bool) -> bool {
    if item.keys().any(|key| !REQUIRED.contains(&key.as_str())) { return true; }
    if !patch && (item.len() != REQUIRED.len() || REQUIRED.iter().any(|key| !item.contains_key(*key))) { return true; }
    for key in ["sku", "name", "description", "category", "status", "currency", "manufacturer", "country_code", "barcode", "color", "size"] {
        if item.contains_key(key) && text(item, key).is_none() { return true; }
    }
    for key in ["price_cents", "stock_count", "weight_grams", "rating_milli"] {
        if item.contains_key(key) && integer(item, key).is_none() { return true; }
    }
    if let Some(value) = item.get("active") { if !matches!(value, Json::Bool(_)) { return true; } }
    if let Some(value) = item.get("tags") {
        match value { Json::Array(values) if values.iter().all(|tag| matches!(tag, Json::Str(_))) => {}, _ => return true }
    }
    if let Some(value) = item.get("metadata") {
        match value {
            Json::Object(metadata) if metadata.len() == 3
                && matches!(metadata.get("source"), Some(Json::Str(_)))
                && matches!(metadata.get("batch"), Some(Json::Str(_)))
                && matches!(metadata.get("fragile"), Some(Json::Bool(_))) => {},
            _ => return true,
        }
    }
    false
}

fn bounded(value: &str, minimum: usize, maximum: usize) -> bool { value.chars().count() >= minimum && value.chars().count() <= maximum }
fn ascii_in(value: &str, allowed: &str) -> bool { value.bytes().all(|byte| allowed.as_bytes().contains(&byte)) }

fn model_error(item: &HashMap<String, Json>) -> Option<&'static str> {
    let sku = text(item, "sku")?;
    if !bounded(sku, 3, 64) || !ascii_in(sku, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") { return Some("sku"); }
    if !bounded(text(item, "name")?, 1, 160) { return Some("name"); }
    if !bounded(text(item, "description")?, 1, 2000) { return Some("description"); }
    if !["hardware", "software", "service", "accessory"].contains(&text(item, "category")?) { return Some("category"); }
    if !["draft", "active", "paused", "retired"].contains(&text(item, "status")?) { return Some("status"); }
    if !(0..=1_000_000_000).contains(&integer(item, "price_cents")?) { return Some("price_cents"); }
    let currency = text(item, "currency")?;
    if currency.len() != 3 || !ascii_in(currency, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") { return Some("currency"); }
    if !(0..=1_000_000).contains(&integer(item, "stock_count")?) { return Some("stock_count"); }
    if !(1..=10_000_000).contains(&integer(item, "weight_grams")?) { return Some("weight_grams"); }
    if !matches!(item.get("active"), Some(Json::Bool(_))) { return Some("active"); }
    if !bounded(text(item, "manufacturer")?, 1, 120) { return Some("manufacturer"); }
    let country = text(item, "country_code")?;
    if country.len() != 2 || !ascii_in(country, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") { return Some("country_code"); }
    let barcode = text(item, "barcode")?;
    if !bounded(barcode, 8, 32) || !ascii_in(barcode, "0123456789") { return Some("barcode"); }
    if !bounded(text(item, "color")?, 1, 40) { return Some("color"); }
    if !bounded(text(item, "size")?, 1, 40) { return Some("size"); }
    if !(0..=5000).contains(&integer(item, "rating_milli")?) { return Some("rating_milli"); }
    match item.get("tags") {
        Some(Json::Array(tags)) if tags.len() <= 16 && tags.iter().all(|tag| matches!(tag, Json::Str(text) if bounded(text, 1, 32))) => {},
        _ => return Some("tags"),
    }
    match item.get("metadata") {
        Some(Json::Object(metadata)) => {
            let source = match metadata.get("source") { Some(Json::Str(value)) => value, _ => return Some("metadata") };
            let batch = match metadata.get("batch") { Some(Json::Str(value)) => value, _ => return Some("metadata") };
            if !bounded(source, 1, 64) || !bounded(batch, 1, 64) { return Some("metadata"); }
        }
        _ => return Some("metadata"),
    }
    None
}

struct Request { method: String, target: String, headers: HashMap<String, String>, body: Vec<u8> }
struct Response { status: u16, body: Option<Json>, headers: Vec<(String, String)> }

fn response(status: u16, body: Option<Json>) -> Response { Response { status, body, headers: Vec::new() } }
fn problem(status: u16, code: &str, field: Option<&str>) -> Response {
    let details = match field {
        Some(field) => Json::Array(vec![object([("field", Json::Str(field.to_string())), ("code", Json::Str("invalid".to_string()))])]),
        None => Json::Array(Vec::new()),
    };
    response(status, Some(object([("error", object([
        ("code", Json::Str(code.to_string())),
        ("message", Json::Str(code.to_string())),
        ("details", details),
    ]))])))
}

fn parse_item(request: &Request, patch: bool) -> Result<HashMap<String, Json>, Response> {
    if !request.headers.get("content-type").map(|value| value.to_ascii_lowercase().starts_with("application/json")).unwrap_or(false) {
        return Err(problem(415, "request_error", None));
    }
    if request.body.len() > 65536 { return Err(problem(413, "request_error", None)); }
    let document = JsonParser::new(&request.body).parse().map_err(|_| problem(400, "request_error", None))?;
    let root = match document { Json::Object(value) => value, _ => return Err(problem(400, "request_error", None)) };
    let item = match root.get("item") { Some(Json::Object(value)) => value.clone(), _ => return Err(problem(400, "request_error", None)) };
    if schema_error(&item, patch) { return Err(problem(400, "request_error", None)); }
    Ok(item)
}

#[derive(Default)]
struct Store { next_id: i64, items: HashMap<i64, HashMap<String, Json>>, skus: HashMap<String, i64> }

fn now_text() -> String {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos().to_string()
}

fn if_match(request: &Request, version: i64) -> Result<(), Response> {
    let text = request.headers.get("if-match").ok_or_else(|| problem(428, "request_error", None))?;
    let candidate: i64 = text.parse().map_err(|_| problem(400, "request_error", None))?;
    if candidate != version { Err(problem(409, "request_error", None)) } else { Ok(()) }
}

fn parse_route(target: &str) -> (&str, Option<&str>) {
    match target.split_once('?') { Some((path, query)) => (path, Some(query)), None => (target, None) }
}

fn authorized(request: &Request) -> bool {
    request.headers.get("host").map(|host| host.starts_with("127.0.0.1") || host.starts_with("localhost") || host.starts_with("[::1]")).unwrap_or(false)
}

fn handle(request: Request, shared: &Arc<Mutex<Store>>) -> Response {
    if !authorized(&request) { return problem(403, "request_error", None); }
    let (path, query) = parse_route(&request.target);
    if path == "/health/ready" && request.method == "GET" {
        return response(200, Some(object([("status", Json::Str("ready".to_string()))])));
    }
    if path == "/v1/items" {
        if request.method == "GET" {
            let status = match query {
                None => None,
                Some(query) if query.starts_with("status=") && !query[7..].contains('&') => Some(&query[7..]),
                _ => return problem(400, "request_error", None),
            };
            if status.map(|value| !["draft", "active", "paused", "retired"].contains(&value)).unwrap_or(false) {
                return problem(422, "validation_failed", Some("status"));
            }
            let store = shared.lock().unwrap();
            let mut values = Vec::new();
            for item in store.items.values() {
                if status.map(|value| text(item, "status") == Some(value)).unwrap_or(true) {
                    values.push(Json::Object(item.clone()));
                }
            }
            let count = values.len();
            values.truncate(100);
            return response(200, Some(object([
                ("data", Json::Array(values)),
                ("meta", object([("count", Json::Int(count as i64)), ("truncated", Json::Bool(count > 100))])),
            ])));
        }
        if request.method == "POST" {
            let mut item = match parse_item(&request, false) { Ok(value) => value, Err(error) => return error };
            if let Some(field) = model_error(&item) { return problem(422, "validation_failed", Some(field)); }
            let sku = text(&item, "sku").unwrap().to_string();
            let mut store = shared.lock().unwrap();
            if store.skus.contains_key(&sku) { return problem(409, "request_error", None); }
            if store.next_id == 0 { store.next_id = 1; }
            let id = store.next_id;
            store.next_id += 1;
            let now = now_text();
            item.insert("id".to_string(), Json::Int(id));
            item.insert("version".to_string(), Json::Int(1));
            item.insert("created_at".to_string(), Json::Str(now.clone()));
            item.insert("updated_at".to_string(), Json::Str(now));
            store.skus.insert(sku, id);
            store.items.insert(id, item.clone());
            let mut result = response(201, Some(Json::Object(item)));
            result.headers.push(("Location".to_string(), format!("/v1/items/{id}")));
            result.headers.push(("ETag".to_string(), "1".to_string()));
            return result;
        }
        return problem(405, "request_error", None);
    }

    let prefix = "/v1/items/";
    if !path.starts_with(prefix) { return problem(404, "request_error", None); }
    let id: i64 = match path[prefix.len()..].parse() { Ok(value) => value, Err(_) => return problem(400, "request_error", None) };
    let snapshot = { shared.lock().unwrap().items.get(&id).cloned() };
    let snapshot = match snapshot { Some(value) => value, None => return problem(404, "request_error", None) };
    if request.method == "GET" {
        let version = integer(&snapshot, "version").unwrap();
        let mut result = response(200, Some(Json::Object(snapshot)));
        result.headers.push(("ETag".to_string(), version.to_string()));
        return result;
    }
    if request.method == "DELETE" {
        let version = integer(&snapshot, "version").unwrap();
        if let Err(error) = if_match(&request, version) { return error; }
        let mut store = shared.lock().unwrap();
        let current = match store.items.remove(&id) { Some(value) => value, None => return problem(404, "request_error", None) };
        store.skus.remove(text(&current, "sku").unwrap());
        return response(204, None);
    }
    if request.method == "PUT" || request.method == "PATCH" {
        let version = integer(&snapshot, "version").unwrap();
        if let Err(error) = if_match(&request, version) { return error; }
        let patch = request.method == "PATCH";
        let incoming = match parse_item(&request, patch) { Ok(value) => value, Err(error) => return error };
        if patch && incoming.is_empty() { return problem(422, "validation_failed", None); }
        let mut candidate = if patch { snapshot.clone() } else { incoming.clone() };
        if patch { for (key, value) in incoming { candidate.insert(key, value); } }
        if let Some(field) = model_error(&candidate) { return problem(422, "validation_failed", Some(field)); }
        let new_sku = text(&candidate, "sku").unwrap().to_string();
        let old_sku = text(&snapshot, "sku").unwrap().to_string();
        let mut store = shared.lock().unwrap();
        if store.skus.get(&new_sku).map(|owner| *owner != id).unwrap_or(false) { return problem(409, "request_error", None); }
        let mut record = HashMap::new();
        for field in REQUIRED { record.insert((*field).to_string(), candidate.get(*field).unwrap().clone()); }
        record.insert("id".to_string(), Json::Int(id));
        record.insert("version".to_string(), Json::Int(version + 1));
        record.insert("created_at".to_string(), snapshot.get("created_at").unwrap().clone());
        record.insert("updated_at".to_string(), Json::Str(now_text()));
        if old_sku != new_sku { store.skus.remove(&old_sku); store.skus.insert(new_sku, id); }
        store.items.insert(id, record.clone());
        let mut result = response(200, Some(Json::Object(record)));
        result.headers.push(("ETag".to_string(), (version + 1).to_string()));
        return result;
    }
    problem(405, "request_error", None)
}

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK", 201 => "Created", 204 => "No Content", 400 => "Bad Request",
        403 => "Forbidden", 404 => "Not Found", 405 => "Method Not Allowed",
        409 => "Conflict", 413 => "Payload Too Large", 415 => "Unsupported Media Type",
        422 => "Unprocessable Content", 428 => "Precondition Required", _ => "Error",
    }
}

fn read_request(reader: &mut BufReader<TcpStream>) -> std::io::Result<Option<Request>> {
    let mut line = String::new();
    if reader.read_line(&mut line)? == 0 { return Ok(None); }
    if line == "\r\n" { line.clear(); if reader.read_line(&mut line)? == 0 { return Ok(None); } }
    let mut parts = line.trim_end().split_whitespace();
    let method = parts.next().unwrap_or("").to_string();
    let target = parts.next().unwrap_or("").to_string();
    let mut headers = HashMap::new();
    loop {
        line.clear();
        if reader.read_line(&mut line)? == 0 { return Ok(None); }
        if line == "\r\n" || line == "\n" { break; }
        if let Some((name, value)) = line.split_once(':') {
            headers.insert(name.trim().to_ascii_lowercase(), value.trim().to_string());
        }
    }
    let length = headers.get("content-length").and_then(|value| value.parse::<usize>().ok()).unwrap_or(0);
    let mut body = vec![0; length];
    reader.read_exact(&mut body)?;
    Ok(Some(Request { method, target, headers, body }))
}

fn write_response(stream: &mut TcpStream, response: Response) -> std::io::Result<()> {
    let mut body = String::new();
    if let Some(value) = response.body { encode(&value, &mut body); }
    write!(stream, "HTTP/1.1 {} {}\r\n", response.status, reason(response.status))?;
    if !body.is_empty() { write!(stream, "Content-Type: application/json\r\n")?; }
    for (name, value) in response.headers { write!(stream, "{}: {}\r\n", name, value)?; }
    write!(stream, "Content-Length: {}\r\n\r\n", body.len())?;
    if !body.is_empty() { stream.write_all(body.as_bytes())?; }
    stream.flush()
}

fn serve(stream: TcpStream, store: Arc<Mutex<Store>>) {
    let mut reader = BufReader::new(stream);
    loop {
        let request = match read_request(&mut reader) { Ok(Some(value)) => value, _ => break };
        let close = request.headers.get("connection").map(|value| value.eq_ignore_ascii_case("close")).unwrap_or(false);
        let response = handle(request, &store);
        if write_response(reader.get_mut(), response).is_err() || close { break; }
    }
}

fn main() {
    let mut host = "127.0.0.1".to_string();
    let mut port = 3400u16;
    let args: Vec<String> = env::args().collect();
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "--host" if index + 1 < args.len() => { host = args[index + 1].clone(); index += 2; }
            "--port" if index + 1 < args.len() => { port = args[index + 1].parse().unwrap(); index += 2; }
            _ => { eprintln!("unknown argument: {}", args[index]); std::process::exit(2); }
        }
    }
    let listener = TcpListener::bind((host.as_str(), port)).unwrap();
    let store = Arc::new(Mutex::new(Store::default()));
    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let shared = Arc::clone(&store);
                thread::spawn(move || serve(stream, shared));
            }
            Err(error) => eprintln!("accept: {error}"),
        }
    }
}
