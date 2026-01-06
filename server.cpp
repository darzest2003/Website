#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>
#include <iomanip>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <pqxx/pqxx>        // PostgreSQL C++ lib
#include <sqlite3.h>        // SQLite3 C API
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
namespace beast = boost::beast;           
namespace http = beast::http;             
namespace net = boost::asio;              
using tcp = boost::asio::ip::tcp;         

using json = nlohmann::json;

// Globals
std::string DATA_DIR = "/var/data";
std::string PUBLIC_DIR = "/app/public";

// Mutex for SQLite thread safety
std::mutex sqlite_mutex;

// --- Utility: Read file contents ---
std::optional<std::string> readFile(const fs::path& path) {
    if (!fs::exists(path) || !fs::is_regular_file(path))
        return std::nullopt;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::stringstream buffer;
    buffer << ifs.rdbuf();
    return buffer.str();
}

// --- Utility: MIME type based on extension ---
std::string mimeType(const std::string& path) {
    auto ext = fs::path(path).extension().string();
    if (ext == ".html") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".txt") return "text/plain";
    if (ext == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

// --- Shipping charge calculation ---
int calculateShipping(double total) {
    return (total >= 5000) ? 0 : 250;
}

// --- URL encode/decode helpers ---
std::string urlEncode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int((unsigned char)c) << std::nouppercase;
        }
    }
    return escaped.str();
}

std::string urlDecode(const std::string& str) {
    std::string ret;
    char ch;
    int i, ii;
    for (i=0; i<str.length(); i++) {
        if (str[i] == '%') {
            if (i + 2 < (int)str.length() && sscanf(str.substr(i+1,2).c_str(), "%x", &ii) == 1) {
                ch = static_cast<char>(ii);
                ret += ch;
                i = i+2;
            }
        }
        else if (str[i] == '+') {
            ret += ' ';
        }
        else {
            ret += str[i];
        }
    }
    return ret;
}

// --- Split query string into key-value pairs ---
std::map<std::string, std::string> splitQuery(const std::string& query) {
    std::map<std::string, std::string> result;
    std::istringstream ss(query);
    std::string token;
    while (std::getline(ss, token, '&')) {
        auto pos = token.find('=');
        if (pos == std::string::npos) continue;
        auto key = token.substr(0, pos);
        auto val = token.substr(pos+1);
        result[key] = urlDecode(val);
    }
    return result;
}

// --- SQLite wrapper for products ---
class ProductDB {
    sqlite3* db = nullptr;

public:
    ProductDB(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db)) {
            std::cerr << "Can't open SQLite DB: " << sqlite3_errmsg(db) << "\n";
            sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("Failed to open SQLite DB");
        }
        // Create products table if not exists
        const char* create_sql = R"(
            CREATE TABLE IF NOT EXISTS products (
                id TEXT PRIMARY KEY,
                title TEXT NOT NULL,
                price REAL NOT NULL,
                stock INTEGER NOT NULL,
                img TEXT NOT NULL
            );
        )";
        char* errmsg = nullptr;
        if (sqlite3_exec(db, create_sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
            std::string err = errmsg ? errmsg : "unknown error";
            sqlite3_free(errmsg);
            throw std::runtime_error("Failed to create products table: " + err);
        }
    }

    ~ProductDB() {
        if (db) sqlite3_close(db);
    }

    // Get all products
    std::vector<json> getAllProducts() {
        std::lock_guard<std::mutex> lock(sqlite_mutex);
        std::vector<json> products;
        const char* sql = "SELECT id, title, price, stock, img FROM products;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                json p;
                p["id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                p["title"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                p["price"] = sqlite3_column_double(stmt, 2);
                p["stock"] = sqlite3_column_int(stmt, 3);
                p["img"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                products.push_back(p);
            }
        }
        sqlite3_finalize(stmt);
        return products;
    }

    // Insert or update product
    bool upsertProduct(const json& p) {
        std::lock_guard<std::mutex> lock(sqlite_mutex);
        const char* sql = R"(
            INSERT INTO products(id, title, price, stock, img)
            VALUES(?, ?, ?, ?, ?)
            ON CONFLICT(id) DO UPDATE SET
              title=excluded.title,
              price=excluded.price,
              stock=excluded.stock,
              img=excluded.img;
        )";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

        sqlite3_bind_text(stmt, 1, p["id"].get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, p["title"].get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, p["price"].get<double>());
        sqlite3_bind_int(stmt, 4, p["stock"].get<int>());
        sqlite3_bind_text(stmt, 5, p["img"].get<std::string>().c_str(), -1, SQLITE_TRANSIENT);

        bool success = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return success;
    }

    // Delete all products
    bool clearAllProducts() {
        std::lock_guard<std::mutex> lock(sqlite_mutex);
        char* errMsg = nullptr;
        if (sqlite3_exec(db, "DELETE FROM products;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
            if (errMsg) {
                std::cerr << "Error deleting products: " << errMsg << std::endl;
                sqlite3_free(errMsg);
            }
            return false;
        }
        return true;
    }

    // Get product by id
    std::optional<json> getProduct(const std::string& id) {
        std::lock_guard<std::mutex> lock(sqlite_mutex);
        const char* sql = "SELECT id, title, price, stock, img FROM products WHERE id=? LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);

        std::optional<json> product;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json p;
            p["id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            p["title"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            p["price"] = sqlite3_column_double(stmt, 2);
            p["stock"] = sqlite3_column_int(stmt, 3);
            p["img"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            product = p;
        }
        sqlite3_finalize(stmt);
        return product;
    }
};

// --- PostgreSQL wrapper for orders/customers ---
class OrderDB {
    pqxx::connection conn_;

public:
    OrderDB(const std::string& connStr) : conn_(connStr) {
        if (!conn_.is_open()) {
            throw std::runtime_error("Cannot open PostgreSQL connection");
        }
        // Initialize tables if not exist
        pqxx::work txn(conn_);
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS customers (
                id SERIAL PRIMARY KEY,
                name TEXT NOT NULL,
                contact TEXT NOT NULL,
                email TEXT,
                address TEXT NOT NULL
            );
        )");
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS orders (
                id SERIAL PRIMARY KEY,
                customer_id INTEGER REFERENCES customers(id),
                product_id TEXT NOT NULL,
                total_amount NUMERIC NOT NULL,
                shipping_charge NUMERIC NOT NULL,
                payment_method TEXT NOT NULL,
                order_date TIMESTAMP NOT NULL DEFAULT NOW()
            );
        )");
        txn.commit();
    }

    bool isConnected() const {
        return conn_.is_open();
    }

    // Insert customer and return customer_id
    int addCustomer(const json& cust) {
        pqxx::work txn(conn_);
        pqxx::result r = txn.exec_params(
            "INSERT INTO customers (name, contact, email, address) VALUES ($1, $2, $3, $4) RETURNING id;",
            cust["name"].get<std::string>(), cust["contact"].get<std::string>(),
            cust.value("email", ""), cust["address"].get<std::string>()
        );
        txn.commit();
        return r[0][0].as<int>();
    }

    // Add order
    bool addOrder(const json& order) {
        if (!isConnected()) return false;
        try {
            pqxx::work txn(conn_);
            int cust_id = addCustomer(order["customer"]);
            if (cust_id < 0) return false;

            txn.exec_params(
                "INSERT INTO orders (customer_id, product_id, total_amount, shipping_charge, payment_method) VALUES ($1, $2, $3, $4, $5);",
                cust_id,
                order["product_id"].get<std::string>(),
                order["total_amount"].get<double>(),
                order["shipping_charge"].get<double>(),
                order["payment_method"].get<std::string>()
            );
            txn.commit();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Add order error: " << e.what() << "\n";
            return false;
        }
    }

    // Get all orders joined with customer info
    std::vector<json> getAllOrders() {
        std::vector<json> orders;
        if (!isConnected()) return orders;
        try {
            pqxx::work txn(conn_);
            pqxx::result r = txn.exec(R"(
                SELECT o.id, c.name, c.contact, c.email, c.address, o.product_id,
                       o.total_amount, o.shipping_charge, o.payment_method, o.order_date
                FROM orders o
                JOIN customers c ON o.customer_id = c.id
                ORDER BY o.order_date DESC;
            )");
            for (auto row : r) {
                json o;
                o["id"] = row["id"].as<int>();
                o["name"] = row["name"].as<std::string>();
                o["contact"] = row["contact"].as<std::string>();
                o["email"] = row["email"].as<std::string>();
                o["address"] = row["address"].as<std::string>();
                o["product_id"] = row["product_id"].as<std::string>();
                o["total_amount"] = row["total_amount"].as<double>();
                o["shipping_charge"] = row["shipping_charge"].as<double>();
                o["payment_method"] = row["payment_method"].as<std::string>();
                o["order_date"] = row["order_date"].as<std::string>();
                orders.push_back(o);
            }
        } catch (const std::exception& e) {
            std::cerr << "Get orders error: " << e.what() << "\n";
        }
        return orders;
    }

    // Get order and customer info by order ID
    std::optional<json> getOrderById(int orderId) {
        try {
            pqxx::work txn(conn_);
            pqxx::result r = txn.exec_params(R"(
                SELECT o.id, c.name, c.contact, c.email, c.address, o.product_id,
                       o.total_amount, o.shipping_charge, o.payment_method, o.order_date
                FROM orders o
                JOIN customers c ON o.customer_id = c.id
                WHERE o.id = $1 LIMIT 1;
            )", orderId);
            if (r.empty()) return std::nullopt;
            auto row = r[0];
            json order;
            order["id"] = row["id"].as<int>();
            order["name"] = row["name"].as<std::string>();
            order["contact"] = row["contact"].as<std::string>();
            order["email"] = row["email"].as<std::string>();
            order["address"] = row["address"].as<std::string>();
            order["product_id"] = row["product_id"].as<std::string>();
            order["total_amount"] = row["total_amount"].as<double>();
            order["shipping_charge"] = row["shipping_charge"].as<double>();
            order["payment_method"] = row["payment_method"].as<std::string>();
            order["order_date"] = row["order_date"].as<std::string>();
            return order;
        } catch (const std::exception& e) {
            std::cerr << "Get order by ID error: " << e.what() << "\n";
            return std::nullopt;
        }
    }
};

// --- Server class for HTTP handling ---
class Server {
    net::io_context ioc_;
    tcp::acceptor acceptor_;
    ProductDB productDB_;
    OrderDB orderDB_;

public:
    Server(const std::string& host, unsigned short port, const std::string& sqlitePath, const std::string& pgConnStr)
    : acceptor_(ioc_, tcp::endpoint(net::ip::make_address(host), port)),
      productDB_(sqlitePath), orderDB_(pgConnStr)
    {
        std::cout << "Server running at " << host << ":" << port << "\n";
        doAccept();
    }

    void run() {
        ioc_.run();
    }

private:
    void doAccept() {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket){
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), productDB_, orderDB_)->start();
                }
                doAccept();
            });
    }

    // Session inner class to handle connection lifecycle
    class Session : public std::enable_shared_from_this<Session> {
        tcp::socket socket_;
        beast::flat_buffer buffer_;
        ProductDB& productDB_;
        OrderDB& orderDB_;
        http::request<http::string_body> req_;
        http::response<http::string_body> res_;

    public:
        Session(tcp::socket socket, ProductDB& pdb, OrderDB& odb)
            : socket_(std::move(socket)), productDB_(pdb), orderDB_(odb) {}

        void start() { readRequest(); }

    private:
        void readRequest() {
            auto self = shared_from_this();
            http::async_read(socket_, buffer_, req_,
                [self](beast::error_code ec, std::size_t bytes_transferred){
                    if (!ec)
                        self->handleRequest();
                    else
                        self->fail(ec, "read");
                });
        }

        void handleRequest() {
            if (req_.method() == http::verb::get) {
                handleGet();
            } else if (req_.method() == http::verb::post) {
                handlePost();
            } else {
                sendResponse(http::status::method_not_allowed, "Method Not Allowed");
            }
        }

        void handleGet() {
            std::string target = std::string(req_.target());

            if (target == "/" || target == "/index.html") {
                serveFile("/index.html", "text/html");
            }
            else if (target == "/admin.html") {
                serveFile("/admin.html", "text/html");
            }
            else if (target.find("/api/products") == 0) {
                handleGetProducts();
            }
            else if (target.find("/api/orders") == 0) {
                handleGetOrders();
            }
            else if (target.find("/api/shippingLabel") == 0) {
                handleShippingLabel();
            }
            else {
                std::string filepath = PUBLIC_DIR + target;
                auto content = readFile(filepath);
                if (content) {
                    sendResponse(http::status::ok, *content, mimeType(filepath));
                } else {
                    sendResponse(http::status::not_found, "Not Found");
                }
            }
        }

        void handlePost() {
            std::string target = std::string(req_.target());
            if (target == "/api/products") {
                handlePostProducts();
            } else if (target == "/api/orders") {
                handlePostOrders();
            } else {
                sendResponse(http::status::not_found, "Not Found");
            }
        }

        void serveFile(const std::string& path, const std::string& mime) {
            auto content = readFile(PUBLIC_DIR + path);
            if (content) {
                sendResponse(http::status::ok, *content, mime);
            } else {
                sendResponse(http::status::not_found, "File not found");
            }
        }

        void handleGetProducts() {
            auto products = productDB_.getAllProducts();
            json response = products;
            sendResponse(http::status::ok, response.dump(), "application/json");
        }

        void handleGetOrders() {
            auto orders = orderDB_.getAllOrders();
            json response = orders;
            sendResponse(http::status::ok, response.dump(), "application/json");
        }

        void handleShippingLabel() {
            auto query_pos = req_.target().find('?');
            if (query_pos == std::string::npos) {
                sendResponse(http::status::bad_request, "Missing order id");
                return;
            }
            auto query_str = req_.target().substr(query_pos + 1);
            auto params = splitQuery(query_str);
            auto it = params.find("id");
            if (it == params.end()) {
                sendResponse(http::status::bad_request, "Missing order id parameter");
                return;
            }
            int orderId = std::stoi(it->second);
            auto orderOpt = orderDB_.getOrderById(orderId);
            if (!orderOpt) {
                sendResponse(http::status::not_found, "Order not found");
                return;
            }
            json order = *orderOpt;
            // Simplified shipping label JSON output
            json label = {
                {"order_id", order["id"]},
                {"name", order["name"]},
                {"contact", order["contact"]},
                {"address", order["address"]},
                {"product_id", order["product_id"]},
                {"order_date", order["order_date"]}
            };
            sendResponse(http::status::ok, label.dump(4), "application/json");
        }

        void handlePostProducts() {
            try {
                auto body_json = json::parse(req_.body());
                if (body_json.is_array()) {
                    productDB_.clearAllProducts();
                    for (const auto& prod : body_json) {
                        productDB_.upsertProduct(prod);
                    }
                    sendResponse(http::status::ok, R"({"message":"Products updated successfully"})", "application/json");
                } else {
                    sendResponse(http::status::bad_request, R"({"error":"Expected JSON array"})", "application/json");
                }
            } catch (json::parse_error& e) {
                sendResponse(http::status::bad_request, R"({"error":"Invalid JSON"})", "application/json");
            }
        }

        void handlePostOrders() {
            try {
                auto body_json = json::parse(req_.body());

                // Basic validation for required fields
                if (!body_json.contains("customer") || !body_json.contains("product_id") || 
                    !body_json.contains("total_amount") || !body_json.contains("payment_method")) {
                    sendResponse(http::status::bad_request, R"({"error":"Missing required order fields"})", "application/json");
                    return;
                }

                // Calculate shipping charge
                double total = body_json["total_amount"].get<double>();
                int shipping_charge = calculateShipping(total);
                body_json["shipping_charge"] = shipping_charge;

                if (orderDB_.addOrder(body_json)) {
                    sendResponse(http::status::ok, R"({"message":"Order placed successfully"})", "application/json");
                } else {
                    sendResponse(http::status::internal_server_error, R"({"error":"Failed to add order"})", "application/json");
                }
            } catch (json::parse_error& e) {
                sendResponse(http::status::bad_request, R"({"error":"Invalid JSON"})", "application/json");
            }
        }

        void sendResponse(http::status statusCode, const std::string& body, const std::string& contentType = "text/plain") {
            res_.version(req_.version());
            res_.result(statusCode);
            res_.set(http::field::server, "Boost.Beast Server");
            res_.set(http::field::content_type, contentType);
            res_.body() = body;
            res_.content_length(body.size());

            auto self = shared_from_this();
            http::async_write(socket_, res_,
                [self](beast::error_code ec, std::size_t){
                    self->socket_.shutdown(tcp::socket::shutdown_send, ec);
                });
        }

        void fail(beast::error_code ec, char const* what) {
            if (ec == net::error::operation_aborted) return;
            std::cerr << what << ": " << ec.message() << "\n";
        }
    };
};
