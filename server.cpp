// server.cpp
// Polished single-file C++ HTTP server for ONLINETRADERZ
// SQLite port of original file-backed server (ready-to-compile).
// Compile with: g++ server.cpp -o server -pthread -lsqlite3

#include <iostream>
#include <functional>
#include <netinet/tcp.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
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
#include <signal.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <sqlite3.h>

using namespace std;

// =================== Configuration & Globals for Enhancements ===================
static atomic<bool> g_running(true);
static int g_server_fd = -1;
static string g_data_dir = "data";
static int g_max_workers = 4;

// =================== Simple structured logging ===================
enum LogLevel { LOG_DEBUG=0, LOG_INFO=1, LOG_WARN=2, LOG_ERROR=3 };

static const char* lvlToStr(LogLevel l) {
switch(l){ case LOG_DEBUG: return "DEBUG"; case LOG_INFO: return "INFO"; case LOG_WARN: return "WARN"; default: return "ERROR"; }
}

static void logMsg(LogLevel lvl, const string &msg) {
using namespace chrono;
auto now = system_clock::now();
time_t tt = system_clock::to_time_t(now);
tm tm;
gmtime_r(&tt, &tm);
char buf[64];
strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
cerr << "[" << buf << "] " << lvlToStr(lvl) << " - " << msg << "\n";
}

#define LOGI(msg) logMsg(LOG_INFO, msg)
#define LOGW(msg) logMsg(LOG_WARN, msg)
#define LOGE(msg) logMsg(LOG_ERROR, msg)
#define LOGD(msg) logMsg(LOG_DEBUG, msg)

// =================== ThreadPool (simple) ===================
class ThreadPool {
public:
ThreadPool(int workers = 4) : stop(false) {
for (int i=0;i<workers;++i) {
threads.emplace_back([this,i]{
while (true) {
function<void()> task;
{
unique_lock<mutex> lock(this->mtx);
this->cv.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
if (this->stop && this->tasks.empty()) return;
task = move(this->tasks.front());
this->tasks.pop();
}
try {
task();
} catch (const exception &ex) {
LOGE(string("Unhandled exception in worker: ") + ex.what());
} catch (...) {
LOGE("Unhandled non-exception thrown in worker");
}
}
});
}
}
~ThreadPool() {
{
unique_lock<mutex> lock(mtx);
stop = true;
}
cv.notify_all();
for (auto &t : threads) if (t.joinable()) t.join();
}
void enqueue(function<void()> f) {
{
unique_lock<mutex> lock(mtx);
if (stop) throw runtime_error("enqueue on stopped ThreadPool");
tasks.push(move(f));
}
cv.notify_one();
}
private:
vector<thread> threads;
queue<function<void()>> tasks;
mutex mtx;
condition_variable cv;
bool stop;
};

// =================== Graceful shutdown handling ===================
static ThreadPool *g_threadpool_ptr = nullptr;
static sqlite3 *g_db = nullptr;
static mutex g_storage_mutex; // simple mutex to guard products/orders vectors & db writes

static void gracefulShutdown(int signo) {
string s = "Received signal ";
s += to_string(signo);
LOGI(s + " - initiating graceful shutdown");
g_running.store(false);
if (g_server_fd >= 0) {
// closing listening socket will unblock accept
close(g_server_fd);
g_server_fd = -1;
}
// allow threadpool destructor to run and finish tasks
}

// =================== End of enhancements; original code begins ===================

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
if (stat(g_data_dir.c_str(), &info) != 0) {
// attempt to create data directory
if (mkdir(g_data_dir.c_str(), 0777) != 0 && errno != EEXIST) {
cerr << "mkdir failed: " << strerror(errno) << "\n";
} else {
LOGI(string("Created data directory: ") + g_data_dir);
}
}
// build path relative to data dir
if (filename.empty()) return g_data_dir + "/";
return g_data_dir + "/" + filename;
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

// --- NEW: read file in binary mode and return as string of bytes ---
string readFileBinary(const string &path) {
ifstream file(path, ios::in | ios::binary);
if (!file.is_open()) return "";
std::ostringstream ss;
ss << file.rdbuf();
return ss.str();
}

// atomic write helper: write to temp file then rename, ensure fsync
bool atomicWriteFile(const string &path, const string &content) {
// ensure parent directory exists
string dir;
size_t pos = path.find_last_of('/');
if (pos == string::npos) dir = "";
else dir = path.substr(0, pos+1);

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

// ------------------- Storage (products & orders) using SQLite -------------------

// Create DB and tables if not exist
bool initDatabase() {
string dbPath = ensureDataFolder("server.db");
int rc = sqlite3_open(dbPath.c_str(), &g_db);
if (rc != SQLITE_OK) {
LOGE(string("Failed to open SQLite database: ") + sqlite3_errmsg(g_db));
if (g_db) sqlite3_close(g_db);
g_db = nullptr;
return false;
}
const char *createSQL =
"BEGIN;"
"CREATE TABLE IF NOT EXISTS products ("
"id TEXT PRIMARY KEY,"
"title TEXT,"
"price REAL,"
"img TEXT,"
"stock INTEGER"
");"
"CREATE TABLE IF NOT EXISTS orders ("
"id TEXT PRIMARY KEY,"
"product TEXT,"
"name TEXT,"
"contact TEXT,"
"email TEXT,"
"address TEXT,"
"productPrice TEXT,"
"deliveryCharges TEXT,"
"totalAmount TEXT,"
"payment TEXT,"
"createdAt TEXT"
");"
"COMMIT;";
char *err = nullptr;
rc = sqlite3_exec(g_db, createSQL, nullptr, nullptr, &err);
if (rc != SQLITE_OK) {
LOGE(string("Failed to create tables: ") + (err ? err : "unknown"));
if (err) sqlite3_free(err);
return false;
}
// Enable WAL mode for better concurrency (best-effort)
sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
return true;
}

// Attempt to migrate existing text files into SQLite if tables are empty
void migrateTextFilesIfNeeded() {
lock_guard<mutex> lock(g_storage_mutex);
if (!g_db) return;

// Check if products table is empty  
int count = 0;  
sqlite3_stmt *stmt = nullptr;  
const char *countProductsSql = "SELECT COUNT(*) FROM products;";  
if (sqlite3_prepare_v2(g_db, countProductsSql, -1, &stmt, nullptr) == SQLITE_OK) {  
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);  
}  
sqlite3_finalize(stmt);  

if (count == 0) {  
    // Try to read products.txt and insert rows  
    string productsPath = ensureDataFolder("products.txt");  
    ifstream pf(productsPath);  
    if (pf.is_open()) {  
        LOGI("Migrating products.txt into SQLite (products table empty)");  
        string line;  
        sqlite3_exec(g_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);  
        const char *insertSQL = "INSERT INTO products (id,title,price,img,stock) VALUES (?,?,?,?,?);";  
        sqlite3_stmt *ins = nullptr;  
        sqlite3_prepare_v2(g_db, insertSQL, -1, &ins, nullptr);  
        while (getline(pf, line)) {  
            if (line.empty()) continue;  
            istringstream iss(line);  
            Product p; string priceStr, stockStr;  
            getline(iss, p.id, '|');  
            getline(iss, p.title, '|');  
            getline(iss, priceStr, '|');  
            try { p.price = priceStr.empty() ? 0.0 : stod(priceStr); } catch(...) { p.price = 0.0; }  
            getline(iss, p.img, '|');  
            getline(iss, stockStr, '|');  
            try { p.stock = stockStr.empty() ? 0 : stoi(stockStr); } catch(...) { p.stock = 0; }  

            sqlite3_bind_text(ins, 1, p.id.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 2, p.title.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_double(ins, 3, p.price);  
            sqlite3_bind_text(ins, 4, p.img.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_int(ins, 5, p.stock);  
            sqlite3_step(ins);  
            sqlite3_reset(ins);  
        }  
        sqlite3_finalize(ins);  
        sqlite3_exec(g_db, "COMMIT;", nullptr, nullptr, nullptr);  
        pf.close();  
    }  
}  

// Check if orders table is empty  
count = 0;  
const char *countOrdersSql = "SELECT COUNT(*) FROM orders;";  
stmt = nullptr;  
if (sqlite3_prepare_v2(g_db, countOrdersSql, -1, &stmt, nullptr) == SQLITE_OK) {  
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);  
}  
sqlite3_finalize(stmt);  

if (count == 0) {  
    string ordersPath = ensureDataFolder("orders.txt");  
    ifstream ofile(ordersPath);  
    if (ofile.is_open()) {  
        LOGI("Migrating orders.txt into SQLite (orders table empty)");  
        string line;  
        sqlite3_exec(g_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);  
        const char *insertSQL = "INSERT INTO orders (id,product,name,contact,email,address,productPrice,deliveryCharges,totalAmount,payment,createdAt) VALUES (?,?,?,?,?,?,?,?,?,?,?);";  
        sqlite3_stmt *ins = nullptr;  
        sqlite3_prepare_v2(g_db, insertSQL, -1, &ins, nullptr);  
        while (getline(ofile, line)) {  
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

            sqlite3_bind_text(ins, 1, o.id.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 2, o.product.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 3, o.name.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 4, o.contact.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 5, o.email.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 6, o.address.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 7, o.productPrice.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 8, o.deliveryCharges.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 9, o.totalAmount.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 10, o.payment.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_bind_text(ins, 11, o.createdAt.c_str(), -1, SQLITE_TRANSIENT);  
            sqlite3_step(ins);  
            sqlite3_reset(ins);  
        }  
        sqlite3_finalize(ins);  
        sqlite3_exec(g_db, "COMMIT;", nullptr, nullptr, nullptr);  
        ofile.close();  
    }  
}

}

// Load products from SQLite into memory
void loadProducts() {
lock_guard<mutex> lock(g_storage_mutex);
products.clear();
if (!g_db) return;
const char *sql = "SELECT id, title, price, img, stock FROM products ORDER BY id;";
sqlite3_stmt *stmt = nullptr;
if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
while (sqlite3_step(stmt) == SQLITE_ROW) {
Product p;
const unsigned char *c0 = sqlite3_column_text(stmt, 0);
const unsigned char *c1 = sqlite3_column_text(stmt, 1);
const unsigned char* c3 = sqlite3_column_text(stmt, 3);
p.id = c0 ? (const char*)c0 : "";
p.title = c1 ? (const char*)c1 : "";
p.price = sqlite3_column_double(stmt, 2);
p.img = c3 ? (const char*)c3 : "";
p.stock = sqlite3_column_int(stmt, 4);
products.push_back(p);
if (p.id.size() > 1 && p.id[0] == 'p') {
try {
int num = stoi(p.id.substr(1));
if (num > currentProductID) currentProductID = num;
} catch (...) {}
}
}
}
sqlite3_finalize(stmt);
}

// Persist in-memory products to DB (simple: delete all and insert)
void saveProducts() {
lock_guard<mutex> lock(g_storage_mutex);
if (!g_db) return;
sqlite3_exec(g_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
sqlite3_exec(g_db, "DELETE FROM products;", nullptr, nullptr, nullptr);
const char *sql = "INSERT INTO products (id, title, price, img, stock) VALUES (?, ?, ?, ?, ?);";
sqlite3_stmt *stmt = nullptr;
if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
for (auto &p : products) {
sqlite3_bind_text(stmt, 1, p.id.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 2, p.title.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_double(stmt, 3, p.price);
sqlite3_bind_text(stmt, 4, p.img.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_int(stmt, 5, p.stock);
sqlite3_step(stmt);
sqlite3_reset(stmt);
}
} else {
LOGE("Failed to prepare insert into products");
}
sqlite3_finalize(stmt);
sqlite3_exec(g_db, "COMMIT;", nullptr, nullptr, nullptr);
}

// Load orders from SQLite into memory
void loadOrders() {
lock_guard<mutex> lock(g_storage_mutex);
orders.clear();
if (!g_db) return;
const char *sql = "SELECT id, product, name, contact, email, address, productPrice, deliveryCharges, totalAmount, payment, createdAt FROM orders ORDER BY id;";
sqlite3_stmt *stmt = nullptr;
if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
while (sqlite3_step(stmt) == SQLITE_ROW) {
Order o;
const unsigned char *c0 = sqlite3_column_text(stmt, 0);
const unsigned char *c1 = sqlite3_column_text(stmt, 1);
const unsigned char* c2 = sqlite3_column_text(stmt, 2);
const unsigned char* c3 = sqlite3_column_text(stmt, 3);
const unsigned char* c4 = sqlite3_column_text(stmt, 4);
const unsigned char* c5 = sqlite3_column_text(stmt, 5);
const unsigned char* c6 = sqlite3_column_text(stmt, 6);
const unsigned char* c7 = sqlite3_column_text(stmt, 7);
const unsigned char* c8 = sqlite3_column_text(stmt, 8);
const unsigned char* c9 = sqlite3_column_text(stmt, 9);
const unsigned char* c10 = sqlite3_column_text(stmt, 10);
o.id = c0 ? (const char*)c0 : "";
o.product = c1 ? (const char*)c1 : "";
o.name = c2 ? (const char*)c2 : "";
o.contact = c3 ? (const char*)c3 : "";
o.email = c4 ? (const char*)c4 : "";
o.address = c5 ? (const char*)c5 : "";
o.productPrice = c6 ? (const char*)c6 : "";
o.deliveryCharges = c7 ? (const char*)c7 : "";
o.totalAmount = c8 ? (const char*)c8 : "";
o.payment = c9 ? (const char*)c9 : "";
o.createdAt = c10 ? (const char*)c10 : "";
orders.push_back(o);
if (o.id.size() > 1 && o.id[0] == 'O') {
try {
int num = stoi(o.id.substr(1));
if (num > currentOrderID) currentOrderID = num;
} catch (...) {}
}
}
}
sqlite3_finalize(stmt);
}
void saveOrder(const Order &o) {
    lock_guard<mutex> lock(g_storage_mutex);
    if (!g_db) return;

    const char *sql =
        "INSERT INTO orders "
        "(id, product, name, contact, email, address, productPrice, deliveryCharges, totalAmount, payment, createdAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = nullptr;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, o.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, o.product.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, o.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, o.contact.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, o.email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, o.address.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, o.productPrice.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, o.deliveryCharges.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, o.totalAmount.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, o.payment.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 11, o.createdAt.c_str(), -1, SQLITE_TRANSIENT);

        sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);
}
// Persist in-memory orders to DB (simple: delete all and insert)
void saveOrders() {
lock_guard<mutex> lock(g_storage_mutex);
if (!g_db) return;
sqlite3_exec(g_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
sqlite3_exec(g_db, "DELETE FROM orders;", nullptr, nullptr, nullptr);
const char *sql = "INSERT INTO orders (id, product, name, contact, email, address, productPrice, deliveryCharges, totalAmount, payment, createdAt) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
sqlite3_stmt *stmt = nullptr;
if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
for (auto &o : orders) {
sqlite3_bind_text(stmt, 1, o.id.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 2, o.product.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 3, o.name.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 4, o.contact.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 5, o.email.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 6, o.address.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 7, o.productPrice.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 8, o.deliveryCharges.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 9, o.totalAmount.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 10, o.payment.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 11, o.createdAt.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_step(stmt);
sqlite3_reset(stmt);
}
} else {
LOGE("Failed to prepare insert into orders");
}
sqlite3_finalize(stmt);
sqlite3_exec(g_db, "COMMIT;", nullptr, nullptr, nullptr);
}

// ------------------- Utilities (unchanged) -------------------
string generateProductID() { return "p" + to_string(++currentProductID); }
// ------------------- Thread-safe Order ID generation -------------------
string generateOrderID() {
    lock_guard<mutex> lock(g_storage_mutex); // protect currentOrderID
    return "O" + to_string(++currentOrderID);
}

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
// note: ignoring send() return here (best-effort)
send(clientSocket, resStr.c_str(), resStr.size(), 0);
}

// --- NEW: send binary response (headers + raw bytes) ---
void sendBinaryResponse(int clientSocket, const string &status, const string &contentType, const string &bodyBytes) {
stringstream response;
response << "HTTP/1.1 " << status << "\r\n";
response << "Content-Type: " << contentType << "\r\n";
response << "Access-Control-Allow-Origin: *\r\n";
response << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
response << "Access-Control-Allow-Headers: Content-Type\r\n";
response << "Content-Length: " << bodyBytes.size() << "\r\n";
response << "Connection: close\r\n\r\n";
string headerStr = response.str();
// send headers
send(clientSocket, headerStr.c_str(), headerStr.size(), 0);
// send body bytes (may contain nulls)
if (!bodyBytes.empty()) {
ssize_t sent = 0;
const char *data = bodyBytes.data();
size_t remaining = bodyBytes.size();
while (remaining > 0) {
ssize_t n = send(clientSocket, data + sent, remaining, 0);
if (n <= 0) break;
sent += n;
remaining -= n;
}
}
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
    out.push_back(c);
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
ss << "<div style=\"width:" << w
   << "px;height:" << h
   << "px;background:#000;display:inline-block;\"></div>\n";
}
ss << "</div>\n";
return ss.str();
}

// ------------------- Request handling (keeps original logic) -------------------
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

// Log request (client IP not available here; kept as previously)  
LOGI(string("Request: ") + method + " " + path);  
LOGD(string("Raw body: [") + body + "]");  

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
    // read-lock by copying products (we hold mutex while copying)  
    {  
        lock_guard<mutex> lock(g_storage_mutex);  
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
    if (p.img.empty()) p.img = "uploads/product1.jpg"; // stored under /public/uploads/  

    {
    lock_guard<mutex> lock(g_storage_mutex);
    products.push_back(p);
    saveProducts();
}

sendResponse(clientSocket, "200 OK", "text/plain", "Product added successfully");
close(clientSocket);
return;

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
    bool deleted = false;  
    {  
        lock_guard<mutex> lock(g_storage_mutex);  
        size_t before = products.size();  
        products.erase(remove_if(products.begin(), products.end(), [&](const Product &p){  
            return trim(p.id) == id;  
        }), products.end());  
        if (products.size() < before) {  
            saveProducts();  
            deleted = true;  
        }  
    }  
    if (deleted) sendResponse(clientSocket, "200 OK", "text/plain", "Product deleted successfully");  
    else sendResponse(clientSocket, "404 Not Found", "text/plain", "Product not found");  
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
    {  
        lock_guard<mutex> lock(g_storage_mutex);  
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

    {  
        lock_guard<mutex> lock(g_storage_mutex);  
        orders.push_back(o);  
        // Persist orders immediately and robustly  
        saveOrders();  
    }  

    // Return order id so frontend can link to shipping label  
    string response = "{\"status\":\"success\",\"message\":\"Order placed successfully\",\"orderId\":\"" + o.id + "\"}";  
    sendResponse(clientSocket, "200 OK", "application/json", response);  
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
    {  
        lock_guard<mutex> lock(g_storage_mutex);  
        for (auto &o : orders) if (trim(o.id) == id) { found = &o; break; }  
    }  
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
    html += generateBarcodeHtml(found->id + "|" + found->createdAt + "|" + found->contact);  
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

// Determine content type first  
string contentType = "text/html";  
string assetLower = assetPath;  
transform(assetLower.begin(), assetLower.end(), assetLower.begin(), ::tolower);  
if (assetLower.find(".css") != string::npos) contentType = "text/css";  
else if (assetLower.find(".js") != string::npos) contentType = "application/javascript";  
else if (assetLower.find(".png") != string::npos) contentType = "image/png";  
else if (assetLower.find(".jpg") != string::npos || assetLower.find(".jpeg") != string::npos) contentType = "image/jpeg";  
else if (assetLower.find(".svg") != string::npos) contentType = "image/svg+xml";  
else if (assetLower.find(".gif") != string::npos) contentType = "image/gif";  
else if (assetLower.find(".webp") != string::npos) contentType = "image/webp";  
else if (assetLower.find(".ico") != string::npos) contentType = "image/x-icon";  
else if (assetLower.find(".bmp") != string::npos) contentType = "image/bmp";  
else if (assetLower.find(".avif") != string::npos) contentType = "image/avif";  

// For images and other binary types, read binary and send with binary sender  
bool isBinary = false;  
if (assetLower.find(".png") != string::npos ||  
    assetLower.find(".jpg") != string::npos ||  
    assetLower.find(".jpeg") != string::npos ||  
    assetLower.find(".gif") != string::npos ||  
    assetLower.find(".webp") != string::npos ||  
    assetLower.find(".ico") != string::npos ||  
    assetLower.find(".bmp") != string::npos ||  
    assetLower.find(".avif") != string::npos) {  
    isBinary = true;  
}  

// Build full path and log it (helps debugging missing files)  
string fullPath = "public" + assetPath;  
LOGI(string("Static request -> ") + fullPath);  

if (isBinary) {  
    string fileContentBin = readFileBinary(fullPath);  
    if (!fileContentBin.empty()) {  
        sendBinaryResponse(clientSocket, "200 OK", contentType, fileContentBin);  
    } else {  
        LOGW(string("Static file not found: ") + fullPath);  
        sendResponse(clientSocket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");  
    }  
} else {  
    string fileContent = readFile(fullPath);  
    if (!fileContent.empty()) {  
        sendResponse(clientSocket, "200 OK", contentType, fileContent);  
    } else {  
        LOGW(string("Static file not found: ") + fullPath);  
        sendResponse(clientSocket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");  
    }  
}  

close(clientSocket);

}

// ------------------- Main -------------------
int main() {
// Enhancement: read env config before continuing
const char *envp_port = getenv("PORT");
const char *env_workers = getenv("MAX_WORKERS");
const char *env_data = getenv("DATA_DIR");

if (env_data && strlen(env_data) > 0) {  
    g_data_dir = string(env_data);  
}  
if (env_workers && strlen(env_workers) > 0) {  
    try { g_max_workers = stoi(string(env_workers)); } catch(...) { g_max_workers = 4; }  
} else {  
    // fallback to hardware-concurrency if available  
    int hc = (int)thread::hardware_concurrency();  
    if (hc > 1) g_max_workers = min(hc, 8);  
}  

// Create data directory if missing (ensure before DB open)  
ensureDataFolder("");  

// Initialize SQLite DB  
if (!initDatabase()) {  
    LOGE("Could not initialize database - exiting");  
    return 1;  
}  

// Migrate any existing text files into the DB if needed  
migrateTextFilesIfNeeded();  

// Load persisted data (from DB now)  
loadProducts();  
loadOrders();  

// Ignore SIGPIPE so send() to closed sockets doesn't kill the process  
signal(SIGPIPE, SIG_IGN);  

// install graceful shutdown handlers  
struct sigaction sa{};  
sa.sa_handler = gracefulShutdown;  
sigemptyset(&sa.sa_mask);  
sa.sa_flags = 0;  
sigaction(SIGINT, &sa, nullptr);  
sigaction(SIGTERM, &sa, nullptr);  

// Setup thread pool (global pointer for destructor on shutdown)  
ThreadPool pool(max(1, g_max_workers));  
g_threadpool_ptr = &pool;  

int server_fd;  
if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {  
    perror("socket failed");  
    if (g_db) sqlite3_close(g_db);  
    return 1;  
}  
// store global so signal handler can close  
g_server_fd = server_fd;  

// Set socket options properly  
int opt = 1;  
if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {  
    perror("setsockopt SO_REUSEADDR failed");  
}

#ifdef SO_REUSEPORT
if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
// non-fatal on systems that don't support it
perror("setsockopt SO_REUSEPORT failed (non-fatal)");
}
#endif

struct sockaddr_in address{};  
address.sin_family = AF_INET;  
address.sin_addr.s_addr = INADDR_ANY;  
int port = 8080;  
if (envp_port) {  
    try { port = stoi(string(envp_port)); } catch(...) { port = 8080; }  
}  
address.sin_port = htons(port);  

if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {  
    perror("bind failed");  
    if (g_db) sqlite3_close(g_db);  
    return 1;  
}  

if (listen(server_fd, 128) < 0) {  
    perror("listen");  
    if (g_db) sqlite3_close(g_db);  
    return 1;  
}  

LOGI(string("🚀 Server running on http://0.0.0.0:") + to_string(port) + " (workers=" + to_string(g_max_workers) + ", data_dir=" + g_data_dir + ")");  

// Accept loop: submit connections to threadpool for handling  
while (g_running.load()) {  
    struct sockaddr_in clientAddr{};  
    socklen_t clientLen = sizeof(clientAddr);  
    int clientSock = accept(server_fd, (struct sockaddr *)&clientAddr, &clientLen);  
    if (clientSock < 0) {  
        if (!g_running.load()) {  
            // shutdown requested  
            break;  
        }  
        perror("accept");  
        continue;  
    }  

    // Set TCP_NODELAY on accepted socket for lower latency  
    int flag = 1;  
    setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));  

    // capture client IP  
    char client_ip[INET_ADDRSTRLEN] = {0};  
    inet_ntop(AF_INET, &clientAddr.sin_addr, client_ip, sizeof(client_ip));  
    string cli = string(client_ip) + ":" + to_string(ntohs(clientAddr.sin_port));  
    LOGI(string("Accepted connection from ") + cli);  

    // Enqueue client handler into threadpool  
    try {  
        pool.enqueue([clientSock, cli]{  
            try {  
                // Wrap original handler and log exceptions  
                handleClient(clientSock);  
            } catch (const exception &ex) {  
                LOGE(string("Exception handling client ") + cli + " : " + ex.what());  
                close(clientSock);  
            } catch (...) {  
                LOGE(string("Unknown exception handling client ") + cli);  
                close(clientSock);  
            }  
        });  
    } catch (const exception &ex) {  
        LOGE(string("Failed to enqueue task: ") + ex.what());  
        close(clientSock);  
    }  
}  

LOGI("Server shutting down, saving data...");  
// Persist data cleanly  
saveOrders();  
saveProducts();  

// ThreadPool destructor will join workers  
g_threadpool_ptr = nullptr;  

if (g_server_fd >= 0) close(g_server_fd);  

if (g_db) {  
    sqlite3_close(g_db);  
    g_db = nullptr;  
    LOGI("Closed SQLite database.");  
}  

LOGI("Shutdown complete");  
return 0;

}
