// (Full updated server.cpp)
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
#include <cstdlib>
#include <iomanip>

// ------------------- Structures -------------------
struct Order {
    std::string product; // product id or summary
    std::string name;
    std::string contact;
    std::string email;
    std::string address;
    std::string productPrice;
    std::string deliveryCharges;
    std::string totalAmount;
    std::string payment;
};

struct Product {
    std::string id;
    std::string title;
    double price;
    std::string img;
    int stock;
};

std::vector<Order> orders;
std::vector<Product> products;
int currentProductID = 0;

// ------------------- Helper Functions -------------------
std::string ensureDataFolder(const std::string &filename) {
    struct stat info;
    if (stat("data", &info) != 0) {
        if (mkdir("data", 0777) != 0 && errno != EEXIST) {
            std::cerr << "mkdir failed: " << strerror(errno) << "\n";
        }
    }
    return filename.empty() ? "data/" : "data/" + filename;
}

std::string trim(const std::string &s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(*start)) start++;
    auto end = s.end();
    do { end--; } while (std::distance(start, end) > 0 && std::isspace(*end));
    return std::string(start, end + 1);
}

static std::string urlDecode(const std::string &src) {
    std::ostringstream out;
    for (size_t i = 0; i < src.length(); ++i) {
        if (src[i] == '+') out << ' ';
        else if (src[i] == '%' && i + 2 < src.length()) {
            int value = 0;
            std::istringstream is(src.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                out << static_cast<char>(value);
                i += 2;
            } else {
                out << '%';
            }
        } else {
            out << src[i];
        }
    }
    return out.str();
}

// ------------------- Orders -------------------
void loadOrders() {
    std::ifstream file(ensureDataFolder("orders.txt"));
    if (!file.is_open()) {
        std::ofstream(ensureDataFolder("orders.txt")).close();
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        Order o;
        std::getline(iss, o.product, '|');
        std::getline(iss, o.name, '|');
        std::getline(iss, o.contact, '|');
        std::getline(iss, o.email, '|');
        std::getline(iss, o.address, '|');
        std::getline(iss, o.productPrice, '|');
        std::getline(iss, o.deliveryCharges, '|');
        std::getline(iss, o.totalAmount, '|');
        std::getline(iss, o.payment, '|');

        double pPrice = 0.0, dCharge = 180.0;
        try { pPrice = o.productPrice.empty() ? 0.0 : std::stod(o.productPrice); } catch (...) { pPrice = 0.0; }
        try { dCharge = o.deliveryCharges.empty() ? 180.0 : std::stod(o.deliveryCharges); } catch (...) { dCharge = 180.0; }
        double tAmount = pPrice + dCharge;

        o.productPrice = std::to_string(pPrice);
        o.deliveryCharges = std::to_string(dCharge);
        o.totalAmount = std::to_string(tAmount);

        orders.push_back(o);
    }
    file.close();
}

void saveOrders() {
    std::ofstream file(ensureDataFolder("orders.txt"), std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "Failed to open orders.txt for writing!\n";
        return;
    }
    for (auto &o : orders) {
        file << o.product << "|" << o.name << "|" << o.contact << "|"
             << o.email << "|" << o.address << "|"
             << o.productPrice << "|" << o.deliveryCharges << "|" << o.totalAmount << "|" << o.payment << "|\n";
    }
    file.close();
}

// ------------------- Products -------------------
void loadProducts() {
    std::string path = ensureDataFolder("products.txt");
    std::ofstream(path, std::ios::app).close();

    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        Product p;
        std::string priceStr, stockStr;
        std::getline(iss, p.id, '|');
        std::getline(iss, p.title, '|');
        std::getline(iss, priceStr, '|');
        try { p.price = priceStr.empty() ? 0.0 : std::stod(priceStr); } catch (...) { p.price = 0.0; }
        std::getline(iss, p.img, '|');
        std::getline(iss, stockStr, '|');
        try { p.stock = stockStr.empty() ? 0 : std::stoi(stockStr); } catch (...) { p.stock = 0; }

        products.push_back(p);

        if (p.id.size() > 1 && p.id[0] == 'p') {
            int num = std::stoi(p.id.substr(1));
            if (num > currentProductID) currentProductID = num;
        }
    }
    file.close();
}

void saveProducts() {
    std::ofstream file(ensureDataFolder("products.txt"), std::ios::trunc);
    if (!file.is_open()) return;
    for (auto &p : products)
        file << p.id << "|" << p.title << "|" << p.price << "|" << p.img << "|" << p.stock << "\n";
    file.close();
}

std::string generateID() { return "p" + std::to_string(++currentProductID); }

// ------------------- JSON Parser -------------------
std::map<std::string, std::string> parseJson(const std::string &body) {
    std::map<std::string, std::string> result;
    std::string key, value;
    enum State { LOOKING_KEY, IN_KEY, LOOKING_VALUE, IN_VALUE } state = LOOKING_KEY;
    bool inString = false;

    for (size_t i = 0; i < body.size(); i++) {
        char c = body[i];
        if (c == '\"') {
            inString = !inString;
            if (!inString && state == IN_KEY) state = LOOKING_VALUE;
            else if (!inString && state == IN_VALUE) {
                result[key] = value;
                key.clear(); value.clear();
                state = LOOKING_KEY;
            } else if (inString && state == LOOKING_KEY) state = IN_KEY;
            else if (inString && state == LOOKING_VALUE) state = IN_VALUE;
        } else {
            if (inString) {
                if (state == IN_KEY) key += c;
                else if (state == IN_VALUE) value += c;
            }
        }
    }
    return result;
}

std::string extractValue(const std::string &body, const std::string &key) {
    auto kv = parseJson(body);
    if (kv.find(key) != kv.end()) return kv[key];
    return "";
}

// ------------------- Utility -------------------
std::string readFile(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void sendResponse(int clientSocket, const std::string &status, const std::string &contentType, const std::string &body) {
    std::stringstream response;
    response << "HTTP/1.1 " << status << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    // CORS headers (allow your Vercel frontend)
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response << "Access-Control-Allow-Headers: Content-Type\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;
    std::string resStr = response.str();
    send(clientSocket, resStr.c_str(), resStr.size(), 0);
}

// parse form-urlencoded body: key1=val1&key2=val2
std::map<std::string,std::string> parseFormUrlEncoded(const std::string &body) {
    std::map<std::string,std::string> out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eq = body.find('=', pos);
        if (eq == std::string::npos) break;
        std::string k = body.substr(pos, eq - pos);
        size_t amp = body.find('&', eq + 1);
        std::string v;
        if (amp == std::string::npos) {
            v = body.substr(eq + 1);
            pos = body.size();
        } else {
            v = body.substr(eq + 1, amp - (eq + 1));
            pos = amp + 1;
        }
        out[k] = urlDecode(v);
    }
    return out;
}

// ------------------- Client Handler -------------------
void handleClient(int clientSocket) {
    // increased buffer to handle larger requests if needed
    std::vector<char> buffer(64 * 1024);
    int bytesReceived = recv(clientSocket, buffer.data(), buffer.size() - 1, 0);
    if (bytesReceived < 0) { close(clientSocket); return; }
    buffer[bytesReceived] = '\0';
    std::string request(buffer.data(), bytesReceived);

    std::istringstream reqStream(request);
    std::string method, path, version;
    reqStream >> method >> path >> version;

    std::cout << "Request: " << method << " " << path << std::endl;

    std::string body = "";
    size_t pos = request.find("\r\n\r\n");
    if (pos != std::string::npos) body = request.substr(pos + 4);

    // Log the raw body for debugging (visible in Render logs)
    std::cout << "Raw body: [" << body << "]" << std::endl;

    // Handle CORS preflight
    if (method == "OPTIONS") {
        sendResponse(clientSocket, "200 OK", "text/plain", "OK");
        close(clientSocket);
        return;
    }

    // ------------------- API Routes -------------------
    if (path.find("/api/login") == 0 && method == "POST") {
        // Try JSON first
        std::string username = extractValue(body, "username");
        std::string password = extractValue(body, "password");

        // If JSON parsing failed, try form-urlencoded
        if (username.empty() && password.empty()) {
            auto form = parseFormUrlEncoded(body);
            if (form.find("username") != form.end()) username = form["username"];
            if (form.find("password") != form.end()) password = form["password"];
        }

        // Trim whitespace
        username = trim(username);
        password = trim(password);

        std::cout << "Login attempt: user='" << username << "'" << std::endl;

        if (username == "admin" && password == "1234") {
            sendResponse(clientSocket, "200 OK", "text/plain", "success");
        } else {
            sendResponse(clientSocket, "401 Unauthorized", "text/plain", "Invalid credentials");
        }
    }
    else if (path.find("/api/orders") == 0 && method == "POST") {
        std::vector<std::pair<std::string,int>> orderProducts;
        std::string bodyStr(body);
        size_t ppos = 0;
        while ((ppos = bodyStr.find("\"product\":", ppos)) != std::string::npos) {
            ppos += 10;
            size_t start = bodyStr.find("\"", ppos) + 1;
            size_t end = bodyStr.find("\"", start);
            if (start == std::string::npos || end == std::string::npos) break;
            std::string prodId = bodyStr.substr(start, end-start);

            size_t qtyPos = bodyStr.find("\"qty\":", end);
            if (qtyPos == std::string::npos) break;
            qtyPos += 6;
            size_t qtyEnd = bodyStr.find_first_of(",}", qtyPos);
            int qty = std::stoi(bodyStr.substr(qtyPos, qtyEnd - qtyPos));

            orderProducts.push_back({prodId, qty});
            ppos = qtyEnd;
        }

        Order o;
        o.name = extractValue(body, "name");
        o.contact = extractValue(body, "contact");
        o.email = extractValue(body, "email");
        o.address = extractValue(body, "address");

        double subtotal = 0.0;
        std::string prodSummary;

        for (auto &p : orderProducts) {
            double price = 0.0;
            std::string title;
            std::string pid = trim(p.first);
            for (auto &prod : products) {
                if (trim(prod.id) == pid) {
                    price = prod.price;
                    title = prod.title;
                    break;
                }
            }
            subtotal += price * p.second;

            char priceStr[20];
            snprintf(priceStr, sizeof(priceStr), "%.2f", price);

            prodSummary += title + " (RS." + std::string(priceStr) + ") x" + std::to_string(p.second) + ", ";
        }
        if (!prodSummary.empty()) prodSummary.pop_back(), prodSummary.pop_back(); // remove last ", "

        double deliveryCharges = 180.0;
        if(subtotal >= 3000 && subtotal < 5000) deliveryCharges = 550;
        else if(subtotal >= 5000) deliveryCharges = 0;

        double totalAmount = subtotal + deliveryCharges;

        char subtotalStr[20], deliveryStr[20], totalStr[20];
        snprintf(subtotalStr, sizeof(subtotalStr), "%.2f", subtotal);
        snprintf(deliveryStr, sizeof(deliveryStr), "%.2f", deliveryCharges);
        snprintf(totalStr, sizeof(totalStr), "%.2f", totalAmount);

        o.product = prodSummary;
        o.productPrice = subtotalStr;
        o.deliveryCharges = deliveryStr;
        o.totalAmount = totalStr;
        o.payment = "Cash on Delivery";

        orders.push_back(o);
        saveOrders();
        sendResponse(clientSocket, "200 OK", "text/plain", "Order placed successfully");
    }
    else if (path.find("/api/orders") == 0 && method == "GET") {
        std::stringstream ss;
        ss << "[";
        for (size_t i = 0; i < orders.size(); ++i) {
            auto &o = orders[i];
            ss << "{"
               << "\"product\":\"" << o.product << "\","
               << "\"name\":\"" << o.name << "\","
               << "\"contact\":\"" << o.contact << "\","
               << "\"email\":\"" << o.email << "\","
               << "\"address\":\"" << o.address << "\","
               << "\"productPrice\":\"" << o.productPrice << "\","
               << "\"deliveryCharges\":\"" << o.deliveryCharges << "\","
               << "\"totalAmount\":\"" << o.totalAmount << "\","
               << "\"payment\":\"" << o.payment << "\""
               << "}";
            if (i != orders.size() - 1) ss << ",";
        }
        ss << "]";
        sendResponse(clientSocket, "200 OK", "application/json", ss.str());
    }
    else if (path.find("/api/products") == 0 && method == "GET") {
        std::stringstream ss;
        ss << "[";
        for (size_t i = 0; i < products.size(); ++i) {
            auto &p = products[i];
            ss << "{"
               << "\"id\":\"" << p.id << "\","
               << "\"title\":\"" << p.title << "\","
               << "\"price\":" << std::fixed << std::setprecision(2) << p.price << ","
               << "\"img\":\"" << p.img << "\","
               << "\"stock\":" << p.stock
               << "}";
            if (i != products.size() - 1) ss << ",";
        }
        ss << "]";
        sendResponse(clientSocket, "200 OK", "application/json", ss.str());
    }
    else if (path.find("/api/addProduct") == 0 && method == "POST") {
        Product p;
        p.id = generateID();
        p.title = extractValue(body, "title");
        std::string priceStr = extractValue(body, "price");
        try { p.price = priceStr.empty() ? 0.0 : std::stod(priceStr); } catch (...) { p.price = 0.0; }
        p.img = extractValue(body, "img");
        std::string stockStr = extractValue(body, "stock");
        try { p.stock = stockStr.empty() ? 0 : std::stoi(stockStr); } catch (...) { p.stock = 0; }

        if (p.img.empty()) p.img = "https://via.placeholder.com/200";
        products.push_back(p);
        saveProducts();
        sendResponse(clientSocket, "200 OK", "text/plain", "Product added successfully");
    }
    else if (path.find("/api/deleteProduct") == 0 && method == "POST") {
        std::string id = extractValue(body, "id");
        auto it = std::remove_if(products.begin(), products.end(), [&](const Product& p){ return p.id == id; });
        if(it != products.end() && it != products.end()) {
            products.erase(it, products.end());
            saveProducts();
            sendResponse(clientSocket, "200 OK", "text/plain", "Product deleted successfully");
        } else {
            sendResponse(clientSocket, "404 Not Found", "text/plain", "Product not found");
        }
    }
    else {
        if (path == "/") path = "/index.html";
        std::string content = readFile("public" + path);
        std::string contentType = "text/html";
        if (path.find(".css") != std::string::npos) contentType = "text/css";
        if (path.find(".js") != std::string::npos) contentType = "application/javascript";
        if (!content.empty())
            sendResponse(clientSocket, "200 OK", contentType, content);
        else
            sendResponse(clientSocket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");
    }

    close(clientSocket);
}

// ------------------- Main -------------------
int main() {
    loadOrders();
    loadProducts();

    int server_fd, clientSocket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    int port = std::stoi(std::getenv("PORT") ? std::getenv("PORT") : "8080");

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    std::cout << "🚀 Server running on http://localhost:" << port << std::endl;

    while (true) {
        if ((clientSocket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
            perror("accept");
            continue;
        }
        handleClient(clientSocket);
    }

    close(server_fd);
    return 0;
}
