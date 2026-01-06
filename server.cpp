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

// Environment variables for PostgreSQL
std::string DB_HOST, DB_PORT, DB_NAME, DB_USER, DB_PASS;

// SQLite DB path for products
std::string SQLITE_DB_PATH;

// Mutex for thread safety on SQLite
std::mutex sqlite_mutex;

// --- Utility: Read file contents (for static files) ---
std::optional<std::string> readFile(const fs::path& path) {
    if (!fs::exists(path) || !fs::is_regular_file(path))
        return std::nullopt;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::stringstream buffer;
    buffer << ifs.rdbuf();
    return buffer.str();
}

// --- Utility: Get MIME type based on extension ---
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

// --- SQLite wrapper for products ---
class ProductDB {
    sqlite3* db = nullptr;

public:
    ProductDB(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db)) {
            std::cerr << "Can't open SQLite DB: " << sqlite3_errmsg(db) << "\n";
            sqlite3_close(db);
            db = nullptr;
        } else {
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
                std::cerr << "Failed to create products table: " << errmsg << "\n";
                sqlite3_free(errmsg);
            }
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

    // Delete product by id
    bool deleteProduct(const std::string& id) {
        std::lock_guard<std::mutex> lock(sqlite_mutex);
        const char* sql = "DELETE FROM products WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        bool success = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return success;
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
    pqxx::connection* conn = nullptr;

public:
    OrderDB(const std::string& connStr) {
        try {
            conn = new pqxx::connection(connStr);
            if (!conn->is_open()) {
                std::cerr << "Cannot open PostgreSQL connection\n";
                delete conn;
                conn = nullptr;
            } else {
                // Initialize tables if not exist
                pqxx::work txn(*conn);
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
        } catch (const std::exception& e) {
            std::cerr << "PostgreSQL connection error: " << e.what() << "\n";
        }
    }
    ~OrderDB() {
        if (conn) conn->close();
        delete conn;
    }

    bool isConnected() const {
        return conn && conn->is_open();
    }

    // Insert customer and return customer_id
    int addCustomer(const json& cust) {
        if (!isConnected()) return -1;
        pqxx::work txn(*conn);
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
            pqxx::work txn(*conn);
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
            pqxx::work txn(*conn);
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
                });
        }

        http::request<http::string_body> req_;
        http::response<http::string_body> res_;

        void handleRequest() {
            // Dispatch based on method and target
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

            // Serve static files
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
                // Try static file from public folder
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

        void serveFile(const std::string& relpath, const std::string& contentType) {
            std::string filepath = PUBLIC_DIR + relpath;
            auto content = readFile(filepath);
            if (!content) {
                sendResponse(http::status::not_found, "File not found");
                return;
            }
            sendResponse(http::status::ok, *content, contentType);
        }

        void sendResponse(http::status status, const std::string& body, const std::string& contentType = "text/plain") {
            res_.version(req_.version());
            res_.keep_alive(false);
            res_.set(http::field::content_type, contentType);
            res_.result(status);
            res_.body() = body;
            res_.prepare_payload();

            auto self = shared_from_this();
            http::async_write(socket_, res_,
                [self](beast::error_code ec, std::size_t){
                    self->socket_.shutdown(tcp::socket::shutdown_send, ec);
                });
        }

        // Handle /api/products GET: Return all products JSON
        void handleGetProducts() {
            auto products = productDB_.getAllProducts();
            json j(products);
            sendResponse(http::status::ok, j.dump(), "application/json");
        }
// Handle /api/products POST: Replace all products (from admin panel)
        void handlePostProducts() {
            try {
                auto body = req_.body();
                auto data = json::parse(body);

                if (!data.is_array()) {
                    sendResponse(http::status::bad_request, "Expected JSON array");
                    return;
                }

                // Clear existing products and insert new
                {
                    // Delete all existing products first
                    // SQLite does not support TRUNCATE, so:
                    std::lock_guard<std::mutex> lock(sqlite_mutex);
                    char* errMsg = nullptr;
                    sqlite3_exec(productDB_.db, "DELETE FROM products;", nullptr, nullptr, &errMsg);
                    if (errMsg) {
                        std::cerr << "Error deleting products: " << errMsg << std::endl;
                        sqlite3_free(errMsg);
                    }
                }

                // Insert all products
                for (const auto& p : data) {
                    if (!p.contains("id") || !p.contains("title") || !p.contains("price") || !p.contains("stock") || !p.contains("img")) {
                        sendResponse(http::status::bad_request, "Invalid product format");
                        return;
                    }
                    bool ok = productDB_.upsertProduct(p);
                    if (!ok) {
                        sendResponse(http::status::internal_server_error, "Failed to save products");
                        return;
                    }
                }
                sendResponse(http::status::ok, "Products updated");
            } catch (const std::exception& e) {
                sendResponse(http::status::bad_request, std::string("Invalid JSON: ") + e.what());
            }
        }

        // Handle /api/orders GET: Return all orders with customer info
        void handleGetOrders() {
            auto orders = orderDB_.getAllOrders();

            // To include product title & img in response, enrich orders:
            auto products = productDB_.getAllProducts();
            std::map<std::string, json> productMap;
            for (const auto& p : products) {
                productMap[p["id"].get<std::string>()] = p;
            }

            json jOrders = json::array();
            for (auto& o : orders) {
                json enriched = o;
                auto pid = o["product_id"].get<std::string>();
                if (productMap.find(pid) != productMap.end()) {
                    enriched["product"] = productMap[pid]["title"];
                    enriched["product_img"] = productMap[pid]["img"];
                } else {
                    enriched["product"] = "Unknown";
                    enriched["product_img"] = "";
                }
                // Include shipping charge in total display if you want, or separate
                jOrders.push_back(enriched);
            }

            sendResponse(http::status::ok, jOrders.dump(), "application/json");
        }

        // Handle /api/orders POST: Place order, send WhatsApp msg, save to DB
        void handlePostOrders() {
            try {
                auto data = json::parse(req_.body());

                // Validate input keys
                if (!data.contains("product") || !data.contains("name") || !data.contains("contact") || !data.contains("email") || !data.contains("address")) {
                    sendResponse(http::status::bad_request, "Missing required fields");
                    return;
                }
                std::string productId = data["product"].get<std::string>();
                std::string name = data["name"].get<std::string>();
                std::string contact = data["contact"].get<std::string>();
                std::string email = data["email"].get<std::string>();
                std::string address = data["address"].get<std::string>();

                // Fetch product info for price and stock check
                auto optProduct = productDB_.getProduct(productId);
                if (!optProduct) {
                    sendResponse(http::status::bad_request, "Invalid product");
                    return;
                }
                auto product = optProduct.value();

                if (product["stock"].get<int>() <= 0) {
                    sendResponse(http::status::bad_request, "Product out of stock");
                    return;
                }

                double price = product["price"].get<double>();
                int stock = product["stock"].get<int>();

                // Calculate shipping charge
                double shippingCharge = calculateShipping(price);

                double totalAmount = price + shippingCharge;

                // Payment method placeholder (you can enhance later)
                std::string paymentMethod = "Cash on Delivery";

                // Save order to DB
                json orderJson = {
                    {"customer", {
                        {"name", name},
                        {"contact", contact},
                        {"email", email},
                        {"address", address}
                    }},
                    {"product_id", productId},
                    {"total_amount", totalAmount},
                    {"shipping_charge", shippingCharge},
                    {"payment_method", paymentMethod}
                };

                if (!orderDB_.addOrder(orderJson)) {
                    sendResponse(http::status::internal_server_error, "Failed to place order");
                    return;
                }

                // Decrement product stock in SQLite DB
                product["stock"] = stock - 1;
                productDB_.upsertProduct(product);

                // Send WhatsApp notification (simulate by returning URL)
                std::string waMsg = "Hello " + name + ", your order for '" + product["title"].get<std::string>() +
                    "' has been received. Total: Rs." + std::to_string(totalAmount) + ". Thank you for shopping with us!";

                // WhatsApp link: wa.me/<countrycode><number>?text=...
                std::string waNumber = contact;
                // Remove any non-digit chars from number, assume Pakistan country code +92 for example:
                waNumber.erase(std::remove_if(waNumber.begin(), waNumber.end(),
                                              [](char c){ return !isdigit(c); }), waNumber.end());
                if (waNumber[0] == '0') waNumber.erase(0,1);
                std::string waUrl = "https://wa.me/92" + waNumber + "?text=" +
                    beast::detail::base64_encode(beast::detail::to_string_view(beast::detail::url_encode(waMsg)));

                // You can return this WhatsApp URL for frontend to open or just text msg
                // For simplicity, return simple message without base64 encoding:
                std::string waTextUrl = "https://wa.me/92" + waNumber + "?text=" + urlEncode(waMsg);

                // Respond success
                sendResponse(http::status::ok, "Order placed successfully! You can send WhatsApp message: " + waTextUrl);

            } catch (const std::exception& e) {
                sendResponse(http::status::bad_request, std::string("Invalid JSON or error: ") + e.what());
            }
        }

        // Shipping label API: /api/shippingLabel?id=order_id
        void handleShippingLabel() {
            auto target = std::string(req_.target());
            auto pos = target.find("?");
            if (pos == std::string::npos) {
                sendResponse(http::status::bad_request, "Missing order id");
                return;
            }
            auto query = target.substr(pos+1);
            std::string orderId;
            for (const auto& kv : splitQuery(query)) {
                if (kv.first == "id") {
                    orderId = kv.second;
                    break;
                }
            }
            if (orderId.empty()) {
                sendResponse(http::status::bad_request, "Missing id parameter");
                return;
            }

            int oid = std::stoi(orderId);
            // Fetch order and customer info from DB
            try {
                pqxx::work txn(*orderDB_.conn);
                pqxx::result r = txn.exec_params(R"(
                    SELECT o.id, c.name, c.contact, c.email, c.address, o.product_id,
                           o.total_amount, o.shipping_charge, o.payment_method, o.order_date
                    FROM orders o
                    JOIN customers c ON o.customer_id = c.id
                    WHERE o.id = $1 LIMIT 1;
                )", oid);
                if (r.empty()) {
                    sendResponse(http::status::not_found, "Order not found");
                    return;
                }
                auto row = r[0];
                std::stringstream label;
                label << "Shipping Label\n";
                label << "-------------------------\n";
                label << "Order ID: " << row["id"].as<int>() << "\n";
                label << "Name: " << row["name"].as<std::string>() << "\n";
                label << "Contact: " << row["contact"].as<std::string>() << "\n";
                label << "Email: " << row["email"].as<std::string>() << "\n";
                label << "Address: " << row["address"].as<std::string>() << "\n";
                label << "Product ID: " << row["product_id"].as<std::string>() << "\n";
                label << "Total Amount: Rs." << row["total_amount"].as<double>() << "\n";
                label << "Shipping Charge: Rs." << row["shipping_charge"].as<double>() << "\n";
                label << "Payment Method: " << row["payment_method"].as<std::string>() << "\n";
                label << "Order Date: " << row["order_date"].as<std::string>() << "\n";

                sendResponse(http::status::ok, label.str(), "text/plain");
            } catch (const std::exception& e) {
                sendResponse(http::status::internal_server_error, "Error fetching shipping label");
            }
        }

        // --- Helpers ---

        static std::map<std::string, std::string> splitQuery(const std::string& query) {
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

        static std::string urlDecode(const std::string& str) {
            std::string ret;
            char ch;
            int i, ii;
            for (i=0; i<str.length(); i++) {
                if (str[i] == '%') {
                    sscanf(str.substr(i+1,2).c_str(), "%x", &ii);
                    ch = static_cast<char>(ii);
                    ret += ch;
                    i = i+2;
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

        static std::string urlEncode(const std::string &value) {
            std::ostringstream escaped;
            escaped.fill('0');
            escaped << std::hex;

            for (char c : value) {
                if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    escaped << c;
                } else {
                    escaped << '%' << std::uppercase << std::setw(2) << int((unsigned char)c) << std::nouppercase;
                }
            }
            return escaped.str();
        }
    };
};

int main() {
    try {
        // Read environment variables for DB and config
        DB_HOST = std::getenv("DB_HOST") ? std::getenv("DB_HOST") : "";
        DB_PORT = std::getenv("DB_PORT") ? std::getenv("DB_PORT") : "5432";
        DB_NAME = std::getenv("DB_NAME") ? std::getenv("DB_NAME") : "";
        DB_USER = std::getenv("DB_USER") ? std::getenv("DB_USER") : "";
        DB_PASS = std::getenv("DB_PASS") ? std::getenv("DB_PASS") : "";

        DATA_DIR = std::getenv("DATA_DIR") ? std::getenv("DATA_DIR") : "/var/data";
        SQLITE_DB_PATH = DATA_DIR + "/products.db";

        std::string pgConnStr = "host=" + DB_HOST +
                                " port=" + DB_PORT +
                                " dbname=" + DB_NAME +
                                " user=" + DB_USER +
                                " password=" + DB_PASS;

        unsigned short port = 8080;

        Server server("0.0.0.0", port, SQLITE_DB_PATH, pgConnStr);
        server.run();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
