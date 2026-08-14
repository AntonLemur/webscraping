#include <iostream>
#include <string>
#include <sstream>
#include <regex>
#include <fstream>
#include <filesystem> // Для создания папок на диске
#include <vector>
#include <thread>  // Для std::jthread (требует C++20)
#include <mutex>   // Для защиты файла CSV от одновременной записи
#include <cpr/cpr.h>  // Подключаем наш HTTP-клиент
#include "gq/Document.h" // Заголовочный файл из gumbo-query
#include "gq/Node.h"

namespace fs = std::filesystem;

std::mutex csv_mutex; // Мьютекс для синхронизации записи в один файл

struct BookData{
    std::string title;
    std::string price;
    std::string img_filename;
};

std::string clean_for_csv(std::string text) {
    size_t pos;
    
    // 1. Удаляем точки с запятой (заменяем на пробел)
    while ((pos = text.find(';')) != std::string::npos) {
        text.replace(pos, 1, " ");
    }
    
    // 2. Удаляем двойные кавычки (заменяем на безопасный одинарный апостроф)
    while ((pos = text.find('"')) != std::string::npos) {
        text.replace(pos, 1, "'");
    }
    
    return text;
}

// Функция, которую будет выполнять каждый отдельный поток для своей книги
void download_book_data(int index, CNode article, const std::string& base_url, const fs::path& img_dir, std::vector<BookData>& results/*, std::ofstream& csv_file*/) {
// 1. Извлекаем текстовые данные (это происходит параллельно в памяти)
    std::string title = "";
    CSelection link_sel = article.find("h3 a");
    if (link_sel.nodeNum() > 0) {
        // Получаем сырой текст и сразу его чистим
        std::string raw_title = link_sel.nodeAt(0).attribute("title");
        title = clean_for_csv(raw_title);
    }

    std::string price = "";
    CSelection price_sel = article.find(".price_color");
    if (price_sel.nodeNum() > 0) {
        price = price_sel.nodeAt(0).text();
    }

    std::string img_filename = "book_" + std::to_string(index + 1) + ".jpg";
    CSelection img_sel = article.find(".image_container img");
    
    if (img_sel.nodeNum() > 0) {
        std::string relative_img_url = img_sel.nodeAt(0).attribute("src");
        std::string full_img_url = base_url + relative_img_url;

        // 2. Скачиваем картинку. Сетевое ожидание теперь происходит параллельно!
        cpr::Response img_response = cpr::Get(cpr::Url{full_img_url});
        
        if (img_response.status_code == 200) {
            std::ofstream img_file(img_dir / img_filename, std::ios::binary);
            if (img_file.is_open()) {
                img_file << img_response.text;
                img_file.close();
            }
        } else {
            img_filename = "error.jpg";
        }
    }

    // 3. ЗАЩИТА ФАЙЛА: Писать в один файл одновременно из разных потоков нельзя — будет каша.
    // Запираем мьютекс. Только один поток в один момент времени может выполнить этот блок кода.
//    {
//        std::lock_guard<std::mutex> lock(csv_mutex);
//        csv_file << title << ";" 
//                 << price << ";" 
//                 << img_filename << "\n";
//    } // Здесь мутекс автоматически открывается (отпирается) для других потоков

    results[index] = {title, price, img_filename};
    std::cout << "[Поток] Успешно обработана книга №" << index + 1 << ": " << title.substr(0, 20) << "...\n";
}

int main() {
    std::string base_url = "https://books.toscrape.com/";
    cpr::Response response = cpr::Get(
        cpr::Url{base_url},
        cpr::Header{{"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"}}
    );

    if (response.status_code == 200) {
        CDocument doc;
        doc.parse(response.text);

        //Количество страниц
        std::string pages = "";
        size_t total_pages = 1;
        CSelection pages_sel = doc.find(".current");
        if (pages_sel.nodeNum() > 0) {
            pages = pages_sel.nodeAt(0).text();
            std::regex pattern(R"(\d+(?=\D*$))");
            std::smatch match;

            if (std::regex_search(pages, match, pattern)) {
                std::stringstream ss;
                ss << match.str();
                ss >> total_pages;
                std::cout << "Страниц: " << total_pages << std::endl;
            } else {
                std::cout << "Одна страница" << std::endl;
            }        
        }

        fs::path img_dir = "covers";
        fs::create_directories(img_dir);

        std::ofstream csv_file("books.csv");
        if (!csv_file.is_open()) return 1;

        // Записываем BOM для Excel и заголовки таблицы
        csv_file << "\xEF\xBB\xBF";
        csv_file << "Название;Цена;Имя файла обложки\n";
        
        size_t current_page = 1;
        std::vector<BookData> results(1000);
        for(;;) {
            CSelection articles = doc.find(".product_pod");
            size_t total_books = articles.nodeNum();
            std::cout << "На странице " << current_page << " найдено книг: " << total_books << ". Запуск многопоточного скачивания...\n";

            // Вектор для хранения наших потоков
            std::vector<std::jthread> workers;

            for (size_t i = 0; i < total_books; ++i) {
                CNode article = articles.nodeAt(i);
                
                // Создаем новый поток jthread, передаем туда рабочую функцию и аргументы.
                // Поток запускается мгновенно и работает параллельно с основным кодом.
                workers.emplace_back(download_book_data, (current_page-1)*total_books + i, article, base_url, std::ref(img_dir), std::ref(results) /*std::ref(csv_file)*/);
            }
            
            if(++current_page > total_pages) break;
            
            std::stringstream ss;
            ss << base_url << "catalogue/page-" << current_page << ".html";
            std::string next_url = ss.str();
            std::cout << "\nПереходим на страницу: " << next_url << "\n";
            response = cpr::Get(cpr::Url{next_url});
            if (response.status_code == 200)
                doc.parse(response.text);
            else
                std::cerr << "Ошибка сети: " << response.status_code << std::endl;
        }

        // Ключевая фишка std::jthread: при выходе вектора из области видимости (или при закрытии main)
        // программа автоматически дождется завершения работы абсолютно всех потоков (деструктор jthread вызывает join).
        std::cout << "Все потоки запущены. Ожидаем завершения...\n";
        
        for(const auto book: results)
            csv_file << book.title << ";" << book.price << ";" << book.img_filename << "\n";
    } else {
        std::cerr << "Ошибка сети: " << response.status_code << std::endl;
    }

    return 0;
}

