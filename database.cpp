#include "database.h"
#include <pqxx/pqxx> // PostgreSQL C++ lib
#include <iostream>
#include <cstdlib>

Database::Database() {}
Database::~Database() {}

bool Database::init() {
    try {
        const char* db_url = getenv("DATABASE_URL");
        if (!db_url) {
            std::cerr << "DATABASE_URL environment variable not set\n";
            return false;
        }
        pqxx::connection C(db_url);
        if (!C.is_open()) {
            std::cerr << "Cannot open database\n";
            return false;
        }
        pqxx::work W(C);

        // Create tables if not exist
        W.exec(R"(CREATE TABLE IF NOT EXISTS products (
                    id TEXT PRIMARY KEY,
                    title TEXT,
                    price REAL,
                    img TEXT,
                    stock INTEGER
                  );)");

        W.exec(R"(CREATE TABLE IF NOT EXISTS orders (
                    id TEXT PRIMARY KEY,
                    product TEXT,
                    name TEXT,
                    contact TEXT,
                    email TEXT,
                    address TEXT,
                    productPrice TEXT,
                    deliveryCharges TEXT,
                    totalAmount TEXT,
                    payment TEXT,
                    createdAt TEXT
                  );)");

        W.commit();
        C.disconnect();
        return true;
    } catch (const std::exception &e) {
        std::cerr << "DB init error: " << e.what() << "\n";
        return false;
    }
}

// TODO: Implement loadProducts, saveProducts, loadOrders, saveOrders
// You can copy your SQLite logic and replace sqlite3 with pqxx
