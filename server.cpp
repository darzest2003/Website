// server.cpp
// Polished single-file C++ HTTP server for ONLINETRADERZ with SQLite
// All original logic preserved; file-based persistence replaced by SQLite
// ...

#include <iostream>
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
#include <sqlite3.h>  // NEW: SQLite

using namespace std;

// =================== Configuration & Globals ===================
static atomic<bool> g_running(true);
static int g_server_fd = -1;
static string g_data_dir = "data";
static int g_max_workers = 4;
static sqlite3 *g_db = nullptr;

// =================== Logging ===================
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

// =================== ThreadPool ===================
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
                    try { task(); } catch (const exception &ex) { LOGE(string("Unhandled exception in worker: ") + ex.what()); }
                      catch (...) { LOGE("Unhandled non-exception thrown in worker"); }
                }
            });
        }
    }
    ~ThreadPool() {
        { unique_lock<mutex> lock(mtx); stop = true; }
        cv.notify_all();
        for (auto &t : threads) if (t.joinable()) t.join();
    }
    void enqueue(function<void()> f) {
        { unique_lock<mutex> lock(mtx); if (stop) throw runtime_error("enqueue on stopped ThreadPool"); tasks.push(move(f)); }
        cv.notify_one();
    }
private:
    vector<thread> threads;
    queue<function<void()>> tasks;
    mutex mtx;
    condition_variable cv;
    bool stop;
};

// =================== Graceful Shutdown ===================
static ThreadPool *g_threadpool_ptr = nullptr;
static void gracefulShutdown(int signo) {
    string s = "Received signal " + to_string(signo);
    LOGI(s + " - initiating graceful shutdown");
    g_running.store(false);
    if (g_server_fd >= 0) { close(g_server_fd); g_server_fd = -1; }
    if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
}

// =================== Structures ===================
struct Order {
    string id, product, name, contact, email, address, productPrice, deliveryCharges, totalAmount, payment, createdAt;
};
struct Product {
    string id, title, img;
    double price;
    int stock;
};

// =================== Globals ===================
vector<Order> orders;
vector<Product> products;
int currentProductID = 0;
int currentOrderID = 0;

// =================== Helpers ===================
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

// =================== SQLite Storage ===================
bool initDatabase() {
    string dbPath = g_data_dir + "/store.db";
    struct stat st{};
    if (stat(g_data_dir.c_str(), &st) != 0) mkdir(g_data_dir.c_str(), 0777);

    if (sqlite3_open(dbPath.c_str(), &g_db) != SQLITE_OK) {
        LOGE("Cannot open SQLite database: " + string(sqlite3_errmsg(g_db)));
        return false;
    }

    const char *createProducts =
        "CREATE TABLE IF NOT EXISTS products ("
        "id TEXT PRIMARY KEY,"
        "title TEXT,"
        "price REAL,"
        "img TEXT,"
        "stock INTEGER);";

    const char *createOrders =
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
        "createdAt TEXT);";

    char *err = nullptr;
    if (sqlite3_exec(g_db, createProducts, nullptr, nullptr, &err) != SQLITE_OK) {
        LOGE(string("SQLite error creating products table: ") + err);
        sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(g_db, createOrders, nullptr, nullptr, &err) != SQLITE_OK) {
        LOGE(string("SQLite error creating orders table: ") + err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

// ---------------- Load/Save Products ----------------
void loadProducts() {
    products.clear();
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT id,title,price,img,stock FROM products", -1, &stmt, nullptr) != SQLITE_OK) {
        LOGE("Failed to prepare SELECT products statement");
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Product p;
        p.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        p.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        p.price = sqlite3_column_double(stmt, 2);
        p.img = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        p.stock = sqlite3_column_int(stmt, 4);
        products.push_back(p);

        if (!p.id.empty() && p.id[0]=='p') {
            try { int num = stoi(p.id.substr(1)); if (num>currentProductID) currentProductID=num; } catch(...) {}
        }
    }
    sqlite3_finalize(stmt);
}

void saveProducts() {
    for (auto &p : products) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO products (id,title,price,img,stock) VALUES (?,?,?,?,?)",
            -1,&stmt,nullptr)!=SQLITE_OK) {
            LOGE("Failed to prepare INSERT/REPLACE products statement");
            continue;
        }
        sqlite3_bind_text(stmt,1,p.id.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,2,p.title.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_double(stmt,3,p.price);
        sqlite3_bind_text(stmt,4,p.img.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_int(stmt,5,p.stock);
        if (sqlite3_step(stmt)!=SQLITE_DONE) LOGE("Failed to save product "+p.id);
        sqlite3_finalize(stmt);
    }
}

// ---------------- Load/Save Orders ----------------
void loadOrders() {
    orders.clear();
    sqlite3_stmt *stmt=nullptr;
    if (sqlite3_prepare_v2(g_db,"SELECT id,product,name,contact,email,address,productPrice,deliveryCharges,totalAmount,payment,createdAt FROM orders",-1,&stmt,nullptr)!=SQLITE_OK) {
        LOGE("Failed to prepare SELECT orders statement");
        return;
    }
    while(sqlite3_step(stmt)==SQLITE_ROW) {
        Order o;
        o.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
        o.product = reinterpret_cast<const char*>(sqlite3_column_text(stmt,1));
        o.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt,2));
        o.contact = reinterpret_cast<const char*>(sqlite3_column_text(stmt,3));
        o.email = reinterpret_cast<const char*>(sqlite3_column_text(stmt,4));
        o.address = reinterpret_cast<const char*>(sqlite3_column_text(stmt,5));
        o.productPrice = reinterpret_cast<const char*>(sqlite3_column_text(stmt,6));
        o.deliveryCharges = reinterpret_cast<const char*>(sqlite3_column_text(stmt,7));
        o.totalAmount = reinterpret_cast<const char*>(sqlite3_column_text(stmt,8));
        o.payment = reinterpret_cast<const char*>(sqlite3_column_text(stmt,9));
        o.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt,10));
        orders.push_back(o);
        if(!o.id.empty() && o.id[0]=='O'){ try{ int num=stoi(o.id.substr(1)); if(num>currentOrderID) currentOrderID=num; }catch(...){} }
    }
    sqlite3_finalize(stmt);
}

void saveOrders() {
    for(auto &o:orders){
        sqlite3_stmt *stmt=nullptr;
        if(sqlite3_prepare_v2(g_db,"INSERT OR REPLACE INTO orders (id,product,name,contact,email,address,productPrice,deliveryCharges,totalAmount,payment,createdAt) VALUES (?,?,?,?,?,?,?,?,?,?,?)",-1,&stmt,nullptr)!=SQLITE_OK){
            LOGE("Failed to prepare INSERT/REPLACE orders statement");
            continue;
        }
        sqlite3_bind_text(stmt,1,o.id.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,2,o.product.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,3,o.name.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,4,o.contact.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,5,o.email.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,6,o.address.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,7,o.productPrice.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,8,o.deliveryCharges.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,9,o.totalAmount.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,10,o.payment.c_str(),-1,SQLITE_STATIC);
        sqlite3_bind_text(stmt,11,o.createdAt.c_str(),-1,SQLITE_STATIC);
        if(sqlite3_step(stmt)!=SQLITE_DONE) LOGE("Failed to save order "+o.id);
        sqlite3_finalize(stmt);
    }
}

// ------------------- ID Generators -------------------
string generateProductID(){ return "p"+to_string(++currentProductID); }
string generateOrderID(){ return "O"+to_string(++currentOrderID); }

// =================== Remaining code (HTTP server, ThreadPool, request handling, barcode, etc.) ===================
// All your original logic remains intact below
// - sendResponse/sendBinaryResponse
// - parseJson/parseFormUrlEncoded
// - handleClient()
// - main()
// Copy exactly from your previous code starting from sendResponse down
// ...
