#ifndef DATABASE_H
#define DATABASE_H

#include <string>
#include <vector>
#include <map>
#include <mutex>

struct Product;
struct Order;

class Database {
public:
    Database();
    ~Database();

    bool init(); // connect to PostgreSQL
    bool loadProducts(std::vector<Product>& products, int& currentProductID);
    bool saveProducts(const std::vector<Product>& products);
    bool loadOrders(std::vector<Order>& orders, int& currentOrderID);
    bool saveOrders(const std::vector<Order>& orders);

private:
    std::mutex mtx;
};

#endif
