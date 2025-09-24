// server.cpp
// Polished single-file C++ HTTP server for ONLINETRADERZ
// - No external dependencies
// - Persistent products & orders stored in data/*.txt
// - Proper save on add/delete
// - Shipping label endpoint that returns printable HTML with a simulated barcode
// - CORS headers included
// - Basic JSON/form parsing compatible with your front-end

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <random>
#include <fcntl.h>

using namespace std;

// ------------------- Structures -------------------
struct Order {
    string id;
    string product; // product summary
    string name;
    string contact;
    string email;
    string address;
    string productPrice;
    string deliveryCharges;
    string totalAmount;
    string payment;
    string createdAt;
};

struct Product {
    string id;
    string title;
    double price;
    string img;
    int stock;
};

// ------------------- Globals -------------------
vector<Order> orders;
vector<Product> products;
int currentProductID = 0;
int currentOrderID = 0;

// ------------------- Helpers -------------------
string ensureDataFolder(const string &filename) {
    struct stat info;
    if (stat("data", &info) != 0) {
        // attempt to create 'data' directory
        if (mkdir("data", 0777) != 0 && errno != EEXIST) {
            cerr << "mkdir failed: " << strerror(errno) << "\n";
        }
    }
    return filename.empty() ? "data/" : string("data/") + filename;
}

string trim(const string &s) {
    auto start = s.begin();
    while (start != s.end() && isspace((unsigned char)*start)) start++;
    auto end = s.end();
    if (start == end) return "";
    do { end--; } while (distance(start, end) > 0 && isspace((unsigned char)*end));
    return string(start, end + 1);
}

string nowISO8601() {
    using namespace chrono;
    auto t = system_clock::now();
    time_t tt = system_clock::to_time_t(t);
    tm tm;
    gmtime_r(&tt, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return string(buf);
}

string readFile(const string &path) {
    ifstream file(path, ios::in);
    if (!file.is_open()) return "";
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// atomic write helper: write to temp file then rename, ensure fsync
bool atomicWriteFile(const string &path, const string &content) {
    // ensure parent directory exists
    string dir = path.substr(0, path.find_last_of('/') + 1);
    if (!dir.empty()) {
        struct stat info;
        if (stat(dir.c_str(), &info) != 0) {
            if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) {
                cerr << "mkdir failed: " << strerror(errno) << "\n";
                // continue; attempt to write anyway
            }
        }
    }

    string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        cerr << "Failed to open temp file for writing: " << tmp << " : " << strerror(errno) << "\n";
        return false;
    }
    ssize_t written = write(fd, content.c_str(), content.size());
    if (written < 0 || (size_t)written != content.size()) {
        cerr << "Failed to write entire content to temp file: " << tmp << " : " << strerror(errno) << "\n";
        close(fd);
        return false;
    }
    // flush to disk
    if (fsync(fd) != 0) {
        cerr << "fsync failed on temp file: " << tmp << " : " << strerror(errno) << "\n";
    }
    close(fd);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        cerr << "rename failed: " << strerror(errno) << "\n";
        // attempt to remove tmp
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// ------------------- Storage (products & orders) -------------------
void loadProducts() {
    products.clear();
    string path = ensureDataFolder("products.txt");
    // ensure exists
    ofstream(path, ios::app).close();

    ifstream file(path);
    if (!file.is_open()) return;

    string line;
    while (getline(file, line)) {
        if (line.empty()) continue;
        istringstream iss(line);
        Product p;
        string priceStr, stockStr;
        getline(iss, p.id, '|');
        getline(iss, p.title, '|');
        getline(iss, priceStr, '|');
        try { p.price = priceStr.empty() ? 0.0 : stod(priceStr); } catch (...) { p.price = 0.0; }
        getline(iss, p.img, '|');
        getline(iss, stockStr, '|');
        try { p.stock = stockStr.empty() ? 0 : stoi(stockStr); } catch (...) { p.stock = 0; }

        products.push_back(p);

        if (p.id.size() > 1 && p.id[0] == 'p') {
            try {
                int num = stoi(p.id.substr(1));
                if (num > currentProductID) currentProductID = num;
            } catch (...) {}
        }
    }
    file.close();
}

void saveProducts() {
    string path = ensureDataFolder("products.txt");
    stringstream ss;
    for (auto &p : products) {
        ss << p.id << "|" << p.title << "|" << fixed << setprecision(2) << p.price << "|" << p.img << "|" << p.stock << "\n";
    }
    string content = ss.str();
    if (!atomicWriteFile(path, content)) {
        // fallback to regular write
        ofstream file(path, ios::trunc);
        if (!file.is_open()) {
            cerr << "Failed to open products.txt for writing!\n";
            return;
        }
        file << content;
        file.close();
    }
}

void loadOrders() {
    orders.clear();
    string path = ensureDataFolder("orders.txt");
    ofstream(path, ios::app).close();
    ifstream file(path);
    if (!file.is_open()) return;
    string line;
    while (getline(file, line)) {
        if (line.empty()) continue;
        istringstream iss(line);
        Order o;
        getline(iss, o.id, '|');
        getline(iss, o.product, '|');
        getline(iss, o.name, '|');
        getline(iss, o.contact, '|');
        getline(iss, o.email, '|');
        getline(iss, o.address, '|');
        getline(iss, o.productPrice, '|');
        getline(iss, o.deliveryCharges, '|');
        getline(iss, o.totalAmount, '|');
        getline(iss, o.payment, '|');
        getline(iss, o.createdAt, '|');
        orders.push_back(o);

        if (o.id.size() > 1 && o.id[0] == 'O') {
            try {
                int num = stoi(o.id.substr(1));
                if (num > currentOrderID) currentOrderID = num;
            } catch (...) {}
        }
    }
    file.close();
}

void saveOrders() {
    string path = ensureDataFolder("orders.txt");
    stringstream ss;
    for (auto &o : orders) {
        ss << o.id << "|" << o.product << "|" << o.name << "|" << o.contact << "|"
           << o.email << "|" << o.address << "|" << o.productPrice << "|" << o.deliveryCharges << "|"
           << o.totalAmount << "|" << o.payment << "|" << o.createdAt << "|\n";
    }
    string content = ss.str();
    if (!atomicWriteFile(path, content)) {
        // fallback to regular write
        ofstream file(path, ios::trunc);
        if (!file.is_open()) {
            cerr << "Failed to open orders.txt for writing!\n";
            return;
        }
        file << content;
        file.close();
    }
}

string generateProductID() { return "p" + to_string(++currentProductID); }
string generateOrderID() { return "O" + to_string(++currentOrderID); }

// ------------------- Simple JSON parser for flat keys -------------------
map<string,string> parseJson(const string &body) {
    map<string,string> res;
    string key, val;
    enum State { NONE, IN_KEY, AFTER_KEY, IN_VAL } st = NONE;
    bool esc = false;
    for (size_t i=0;i<body.size();++i) {
        char c = body[i];
        if (st == NONE) {
            if (c == '"') { st = IN_KEY; key.clear(); esc=false; }
        } else if (st == IN_KEY) {
            if (c == '"' && !esc) { st = AFTER_KEY; }
            else {
                if (c == '\\' && !esc) esc = true; else { key.push_back(c); esc=false; }
            }
        } else if (st == AFTER_KEY) {
            if (c == ':') {
                // move to value; find starting quote
                size_t j = i+1;
                while (j < body.size() && isspace((unsigned char)body[j])) j++;
                if (j < body.size() && body[j] == '"') { st = IN_VAL; i = j; val.clear(); esc=false; }
                else {
                    // non-string value (number, boolean) - capture until comma or }
                    size_t k = j;
                    while (k < body.size() && body[k] != ',' && body[k] != '}' && body[k] != '\n' && body[k] != '\r') k++;
                    string raw = body.substr(j, k-j);
                    // trim
                    size_t a = raw.find_first_not_of(" \t\n\r");
                    size_t b = raw.find_last_not_of(" \t\n\r");
                    if (a==string::npos) res[key]=""; else res[key]=raw.substr(a, b-a+1);
                    i = k-1;
                    st = NONE;
                }
            }
        } else if (st == IN_VAL) {
            if (c == '"' && !esc) {
                // value ended
                res[key] = val;
                st = NONE;
            } else {
                if (c == '\\' && !esc) esc = true; else { val.push_back(c); esc=false; }
            }
        }
    }
    return res;
}

// parse application/x-www-form-urlencoded
string urlDecode(const string &src) {
    ostringstream out;
    for (size_t i=0;i<src.size();++i) {
        if (src[i] == '+') out << ' ';
        else if (src[i] == '%' && i+2 < src.size()) {
            string hex = src.substr(i+1,2);
            char ch = (char) strtol(hex.c_str(), nullptr, 16);
            out << ch;
            i += 2;
        } else out << src[i];
    }
    return out.str();
}
map<string,string> parseFormUrlEncoded(const string &body) {
    map<string,string> res;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eq = body.find('=', pos);
        if (eq == string::npos) break;
        string k = body.substr(pos, eq-pos);
        size_t amp = body.find('&', eq+1);
        string v;
        if (amp == string::npos) { v = body.substr(eq+1); pos = body.size(); }
        else { v = body.substr(eq+1, amp-(eq+1)); pos = amp+1; }
        res[urlDecode(k)] = urlDecode(v);
    }
    return res;
}

// ------------------- Utility / HTTP -------------------
void sendResponse(int clientSocket, const string &status, const string &contentType, const string &body) {
    stringstream response;
    response << "HTTP/1.1 " << status << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response << "Access-Control-Allow-Headers: Content-Type\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;
    string resStr = response.str();
    send(clientSocket, resStr.c_str(), resStr.size(), 0);
}

string getQueryParam(const string &path, const string &key) {
    size_t q = path.find('?');
    if (q == string::npos) return "";
    string qs = path.substr(q+1);
    auto params = parseFormUrlEncoded(qs);
    if (params.find(key) != params.end()) return params[key];
    return "";
}

string htmlEscape(const string &s) {
    string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

// Simulated barcode generator (returns HTML of vertical bars)
string generateBarcodeHtml(const string &seed) {
    // create deterministic pseudo-random bars from seed
    unsigned long hash = 1469598103934665603ULL;
    for (char c : seed) hash = (hash ^ (unsigned long)c) * 1099511628211ULL;
    // create pattern
    stringstream ss;
    ss << "<div style=\"display:flex;align-items:flex-end;height:80px;gap:2px;padding:8px;background:#fff;border:1px solid #ddd;\">\n";
    // produce 40 bars
    for (int i=0;i<40;i++) {
        // deterministic pseudo-random height based on hash and i
        hash = hash * 6364136223846793005ULL + 1442695040888963407ULL + i;
        int h = (int)(20 + (hash % 60)); // 20..79
        int w = (int)(2 + (hash % 4));   // 2..5 px
        ss << "<div style=\"width:" << w << "px;height:" << h << "px;background:#000;display:inline-block;\"></div>\n";
    }
    ss << "</div>\n";
    return ss.str();
}

// ------------------- Request handling -------------------
void handleClient(int clientSocket) {
    const int BUF_SIZE = 8192;
    string request;
    char buffer[BUF_SIZE];
    ssize_t n;

    // Read headers first (robust)
    bool headersDone = false;
    while (!headersDone) {
        n = recv(clientSocket, buffer, BUF_SIZE, 0);
        if (n <= 0) { close(clientSocket); return; }
        request.append(buffer, buffer + n);
        if (request.find("\r\n\r\n") != string::npos) { headersDone = true; break; }
    }

    size_t headerPos = request.find("\r\n\r\n");
    string headers = (headerPos != string::npos) ? request.substr(0, headerPos) : "";
    string body = (headerPos != string::npos) ? request.substr(headerPos + 4) : "";

    // find Content-Length
    size_t contentLength = 0;
    string headersLower = headers;
    transform(headersLower.begin(), headersLower.end(), headersLower.begin(), ::tolower);
    size_t clPos = headersLower.find("content-length:");
    if (clPos != string::npos) {
        size_t start = clPos + strlen("content-length:");
        while (start < headersLower.size() && isspace((unsigned char)headersLower[start])) start++;
        size_t end = headersLower.find("\r\n", start);
        string num = headersLower.substr(start, (end==string::npos?headersLower.size():end)-start);
        try { contentLength = stoul(num); } catch(...) { contentLength = 0; }
    }

    // read remaining body if any
    while (body.size() < contentLength) {
        n = recv(clientSocket, buffer, BUF_SIZE, 0);
        if (n <= 0) break;
        body.append(buffer, buffer + n);
    }

    // parse request line
    istringstream reqStream(request);
    string method, path, version;
    reqStream >> method >> path >> version;
    if (path.empty()) path = "/";

    cerr << "Request: " << method << " " << path << "\n";
    cerr << "Raw body: [" << body << "]\n";

    // quick CORS preflight
    if (method == "OPTIONS") {
        sendResponse(clientSocket, "200 OK", "text/plain", "OK");
        close(clientSocket);
        return;
    }

    // ------------------- API Routes -------------------

    // POST /api/login
    if (path.find("/api/login") == 0 && method == "POST") {
        auto kv = parseJson(body);
        string username = trim(kv.count("username") ? kv["username"] : "");
        string password = trim(kv.count("password") ? kv["password"] : "");
        // fallback to form
        if (username.empty() && password.empty()) {
            auto form = parseFormUrlEncoded(body);
            if (form.count("username")) username = trim(form["username"]);
            if (form.count("password")) password = trim(form["password"]);
        }
        if (username == "admin" && password == "1234") {
            sendResponse(clientSocket, "200 OK", "text/plain", "success");
        } else {
            sendResponse(clientSocket, "401 Unauthorized", "text/plain", "Invalid credentials");
        }
        close(clientSocket);
        return;
    }

    // GET /api/products
    if (path.find("/api/products") == 0 && method == "GET") {
        stringstream ss;
        ss << "[";
        for (size_t i=0;i<products.size();++i) {
            auto &p = products[i];
            ss << "{"
               << "\"id\":\"" << htmlEscape(p.id) << "\","
               << "\"title\":\"" << htmlEscape(p.title) << "\","
               << "\"price\":" << fixed << setprecision(2) << p.price << ","
               << "\"img\":\"" << htmlEscape(p.img) << "\","
               << "\"stock\":" << p.stock
               << "}";
            if (i+1<products.size()) ss << ",";
        }
        ss << "]";
        sendResponse(clientSocket, "200 OK", "application/json", ss.str());
        close(clientSocket);
        return;
    }

    // POST /api/addProduct
    if (path.find("/api/addProduct") == 0 && method == "POST") {
        auto kv = parseJson(body);
        if (kv.empty()) kv = parseFormUrlEncoded(body);
        Product p;
        p.id = generateProductID();
        p.title = trim(kv.count("title") ? kv["title"] : "");
        try { p.price = kv.count("price") ? stod(kv["price"]) : 0.0; } catch(...) { p.price = 0.0; }
        p.img = kv.count("img") ? kv["img"] : "";
        try { p.stock = kv.count("stock") ? stoi(kv["stock"]) : 0; } catch(...) { p.stock = 0; }

        if (p.title.empty()) {
            sendResponse(clientSocket, "400 Bad Request", "application/json", "{\"status\":\"error\",\"message\":\"Title required\"}");
            close(clientSocket);
            return;
        }
        if (p.img.empty()) p.img = "https://via.placeholder.com/200";

        products.push_back(p);
        saveProducts();
        sendResponse(clientSocket, "200 OK", "text/plain", "Product added successfully");
        close(clientSocket);
        return;
    }

    // POST /api/deleteProduct
    if (path.find("/api/deleteProduct") == 0 && method == "POST") {
        auto kv = parseJson(body);
        if (kv.empty()) kv = parseFormUrlEncoded(body);
        string id = trim(kv.count("id") ? kv["id"] : "");
        if (id.empty()) {
            sendResponse(clientSocket, "400 Bad Request", "text/plain", "id required");
            close(clientSocket);
            return;
        }
        size_t before = products.size();
        products.erase(remove_if(products.begin(), products.end(), [&](const Product &p){
            return trim(p.id) == id;
        }), products.end());
        if (products.size() < before) {
            // ensure we write updated product list immediately
            saveProducts();
            sendResponse(clientSocket, "200 OK", "text/plain", "Product deleted successfully");
        } else {
            sendResponse(clientSocket, "404 Not Found", "text/plain", "Product not found");
        }
        close(clientSocket);
        return;
    }

    // POST /api/orders
    if (path.find("/api/orders") == 0 && method == "POST") {
        // Accept JSON body that contains products (array of {product,qty}), plus name/contact/email/address
        // We will compute subtotal using server-side product prices to avoid client manipulation
        string bodyStr = body;
        auto kv = parseJson(bodyStr);
        // fallback to form
        if (kv.empty()) kv = parseFormUrlEncoded(bodyStr);

        // parse products array by scanning body string (simple approach)
        vector<pair<string,int>> orderProducts;
        size_t pos = 0;
        while ((pos = bodyStr.find("\"product\":", pos)) != string::npos) {
            pos += 10;
            size_t start = bodyStr.find('"', pos);
            if (start == string::npos) break;
            start++;
            size_t end = bodyStr.find('"', start);
            if (end == string::npos) break;
            string prodId = bodyStr.substr(start, end-start);

            size_t qtyPos = bodyStr.find("\"qty\":", end);
            if (qtyPos == string::npos) break;
            qtyPos += 6;
            size_t qtyEnd = bodyStr.find_first_of(",}", qtyPos);
            if (qtyEnd == string::npos) break;
            int qty = 1;
            try { qty = stoi(bodyStr.substr(qtyPos, qtyEnd-qtyPos)); } catch(...) { qty = 1; }

            orderProducts.push_back({prodId, qty});
            pos = qtyEnd;
        }

        Order o;
        o.id = generateOrderID();
        o.name = kv.count("name") ? kv["name"] : "";
        o.contact = kv.count("contact") ? kv["contact"] : "";
        o.email = kv.count("email") ? kv["email"] : "";
        o.address = kv.count("address") ? kv["address"] : "";

        double subtotal = 0.0;
        string prodSummary;
        for (auto &pp : orderProducts) {
            string pid = trim(pp.first);
            int qty = pp.second;
            double price = 0.0;
            string title = pid;
            for (auto &prod : products) {
                if (trim(prod.id) == pid) {
                    price = prod.price;
                    title = prod.title;
                    break;
                }
            }
            subtotal += price * qty;
            char priceBuf[64]; snprintf(priceBuf, sizeof(priceBuf), "%.2f", price);
            prodSummary += title + " (RS." + string(priceBuf) + ") x" + to_string(qty) + ", ";
        }
        if (!prodSummary.empty()) { prodSummary.pop_back(); prodSummary.pop_back(); }

        double deliveryCharges = 180.0;
        if (subtotal >= 3000 && subtotal < 5000) deliveryCharges = 550.0;
        else if (subtotal >= 5000) deliveryCharges = 0.0;

        double totalAmount = subtotal + deliveryCharges;
        char subS[32], delS[32], totS[32];
        snprintf(subS, sizeof(subS), "%.2f", subtotal);
        snprintf(delS, sizeof(delS), "%.2f", deliveryCharges);
        snprintf(totS, sizeof(totS), "%.2f", totalAmount);

        o.product = prodSummary.empty() ? "Unknown items" : prodSummary;
        o.productPrice = subS;
        o.deliveryCharges = delS;
        o.totalAmount = totS;
        o.payment = "Cash on Delivery";
        o.createdAt = nowISO8601();

        orders.push_back(o);
        // Persist orders immediately and robustly
        saveOrders();

        // Return order id so frontend can link to shipping label
        string response = "{\"status\":\"success\",\"message\":\"Order placed successfully\",\"orderId\":\"" + o.id + "\"}";
        sendResponse(clientSocket, "200 OK", "application/json", response);
        close(clientSocket);
        return;
    }

    // GET /api/orders
    if (path.find("/api/orders") == 0 && method == "GET") {
        stringstream ss;
        ss << "[";
        for (size_t i=0;i<orders.size();++i) {
            auto &o = orders[i];
            ss << "{"
               << "\"id\":\"" << htmlEscape(o.id) << "\","
               << "\"product\":\"" << htmlEscape(o.product) << "\","
               << "\"name\":\"" << htmlEscape(o.name) << "\","
               << "\"contact\":\"" << htmlEscape(o.contact) << "\","
               << "\"email\":\"" << htmlEscape(o.email) << "\","
               << "\"address\":\"" << htmlEscape(o.address) << "\","
               << "\"productPrice\":\"" << htmlEscape(o.productPrice) << "\","
               << "\"deliveryCharges\":\"" << htmlEscape(o.deliveryCharges) << "\","
               << "\"totalAmount\":\"" << htmlEscape(o.totalAmount) << "\","
               << "\"payment\":\"" << htmlEscape(o.payment) << "\","
               << "\"createdAt\":\"" << htmlEscape(o.createdAt) << "\""
               << "}";
            if (i+1<orders.size()) ss << ",";
        }
        ss << "]";
        sendResponse(clientSocket, "200 OK", "application/json", ss.str());
        close(clientSocket);
        return;
    }

    // GET /api/shippingLabel?id=ORDER_ID
    if (path.find("/api/shippingLabel") == 0 && method == "GET") {
        string id = getQueryParam(path, "id");
        if (id.empty()) {
            sendResponse(clientSocket, "400 Bad Request", "text/plain", "id query param required");
            close(clientSocket);
            return;
        }
        // find order
        Order *found = nullptr;
        for (auto &o : orders) if (trim(o.id) == id) { found = &o; break; }
        if (!found) {
            sendResponse(clientSocket, "404 Not Found", "text/plain", "Order not found");
            close(clientSocket);
            return;
        }
        // Build a simple printable HTML page with order details and simulated barcode
        string html;
        html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>\n";
        html += "<title>Shipping Label - " + htmlEscape(found->id) + "</title>\n";
        html += "<style>body{font-family:Arial,Helvetica,sans-serif;padding:18px;background:#f6f7fb} .label{max-width:720px;margin:0 auto;background:#fff;padding:18px;border-radius:8px;box-shadow:0 10px 30px rgba(0,0,0,0.08)} h1{margin:0 0 8px;font-size:18px} .meta{margin:10px 0} .meta div{margin:4px 0} .barcode-wrap{margin:12px 0;padding:8px;background:#fff;border-radius:6px;display:flex;justify-content:center}\n";
        html += "@media print{body{background:#fff} .label{box-shadow:none}}</style></head><body>\n";
        html += "<div class='label'>\n";
        html += "<h1>ONLINETRADERZ — Shipping Label</h1>\n";
        html += "<div class='meta'><div><strong>Order ID:</strong> " + htmlEscape(found->id) + "</div>\n";
        html += "<div><strong>Customer:</strong> " + htmlEscape(found->name) + "</div>\n";
        html += "<div><strong>Contact:</strong> " + htmlEscape(found->contact) + "</div>\n";
        html += "<div><strong>Address:</strong> " + htmlEscape(found->address) + "</div>\n";
        html += "<div><strong>Items:</strong> " + htmlEscape(found->product) + "</div>\n";
        html += "<div><strong>Total:</strong> RS." + htmlEscape(found->totalAmount) + "</div>\n";
        html += "</div>\n";
        html += "<div class='barcode-wrap'>\n";
        html += generateBarcodeHtml(found->id + \"|\" + found->createdAt + \"|\" + found->contact);
        html += "</div>\n";
        html += "<div style='text-align:center;margin-top:14px;color:#666;font-size:12px'>Printed: " + nowISO8601() + "</div>\n";
        html += "</div>\n</body></html>";
        sendResponse(clientSocket, "200 OK", "text/html", html);
        close(clientSocket);
        return;
    }

    // Serve static files from public/ (fallback)
    string assetPath = path;
    if (assetPath == "/") assetPath = "/index.html";
    string fileContent = readFile("public" + assetPath);
    string contentType = "text/html";
    if (assetPath.find(".css") != string::npos) contentType = "text/css";
    else if (assetPath.find(".js") != string::npos) contentType = "application/javascript";
    else if (assetPath.find(".png") != string::npos) contentType = "image/png";
    else if (assetPath.find(".jpg") != string::npos || assetPath.find(".jpeg") != string::npos) contentType = "image/jpeg";
    else if (assetPath.find(".svg") != string::npos) contentType = "image/svg+xml";

    if (!fileContent.empty()) {
        sendResponse(clientSocket, "200 OK", contentType, fileContent);
    } else {
        sendResponse(clientSocket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");
    }
    close(clientSocket);
}

// ------------------- Main -------------------
int main() {
    // Load persisted data
    loadProducts();
    loadOrders();

    // Create data directory if missing
    ensureDataFolder("");

    int server_fd;
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    int port = 8080;
    const char *envp = getenv("PORT");
    if (envp) {
        try { port = stoi(string(envp)); } catch(...) { port = 8080; }
    }
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    cout << "🚀 Server running on http://localhost:" << port << endl;

    while (true) {
        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientSock = accept(server_fd, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientSock < 0) {
            perror("accept");
            continue;
        }
        // simple sequential handling — fine for small loads; replace with threads/processes if needed
        handleClient(clientSock);
    }

    close(server_fd);
    return 0;
}
