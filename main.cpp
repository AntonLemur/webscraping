#include <iostream>
#include <string>
#include <sstream>
#include <regex>
#include <fstream>
#include <filesystem> // Для создания папок на диске
#include <vector>
#include <thread>  // Для std::jthread (требует C++20)
#include <cpr/cpr.h>  // Подключаем наш HTTP-клиент
#include "gq/Document.h" // Заголовочный файл из gumbo-query
#include "gq/Node.h"

namespace fs = std::filesystem;

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
void download_entire_page(int page_num, int books_per_page, const std::string& base_url, const fs::path& img_dir, std::vector<BookData>& results) {
    // 1. Поток САМ формирует URL страницы и скачивает её HTML
    std::string page_url = base_url + "catalogue/page-" + std::to_string(page_num) + ".html";
//    if (page_num == 1) page_url = base_url + "index.html"; // костыль для первой страницы

    cpr::Response response = cpr::Get(cpr::Url{page_url});
    if (response.status_code != 200) return;

    // 2. Поток САМ парсит HTML этой страницы
    CDocument doc;
    doc.parse(response.text);
    CSelection articles = doc.find(".product_pod");

    // 3. Поток циклом обходит 20 книг строго своей страницы
    for (size_t i = 0; i < articles.nodeNum(); ++i) {
        CNode article = articles.nodeAt(i);
        
        // Глобальный индекс теперь зависит от переменной, а не от магического числа 20
        int global_index = (page_num - 1) * books_per_page + i;
    
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

        std::string img_filename = "book_" + std::to_string(global_index + 1) + ".jpg";
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

        results[global_index] = {title, price, img_filename};
        std::cout << "[Поток] Успешно обработана книга №" << global_index + 1 << ": " << title.substr(0, 20) << "...\n";
    }
}

int main() {
    std::string base_url = "https://books.toscrape.com/";

    // 1. СТАРТОВЫЙ ЗАПРОС: скачиваем только первую страницу для разведки
    cpr::Response response = cpr::Get(
        cpr::Url{base_url},
        cpr::Header{{"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"}}
    );

    if (response.status_code != 200) {
        std::cerr << "Не удалось подключиться к сайту!" << std::endl;
        return 1;
    }

    CDocument doc;
    doc.parse(response.text);

    // 2. ДИНАМИЧЕСКИЙ РАСЧЕТ ПАРАМЕТРОВ (Никакого хардкода!)
    int books_per_page = doc.find(".product_pod").nodeNum(); // Считаем книги на 1-й странице (выдаст 20)

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
        }        
    }

    std::cout << "[Инфо] Книг на странице: " << books_per_page << "\n";
    std::cout << "[Инфо] Всего страниц на сайте: " << total_pages << "\n";

    // 3. Выделяем точный объем памяти в векторе
    int total_expected_books = total_pages * books_per_page;
    std::vector<BookData> results(total_expected_books);

    // 4. Запускаем пул потоков
    std::vector<std::jthread> workers;
    fs::path img_dir = "covers";
    fs::create_directories(img_dir);

    for (int page = 1; page <= total_pages; ++page) {
        // Передаем books_per_page внутрь потока, чтобы он правильно считал глобальный индекс
        workers.emplace_back(download_entire_page, page, books_per_page, base_url, std::ref(img_dir), std::ref(results));
    }
    
    // Ожидаем завершения всех параллельных задач
    workers.clear(); 

   // 5. Запись в CSV (проходим только по реально заполненным элементам)
    std::ofstream csv_file("books.csv");
    if (!csv_file.is_open()) return 1;

    // Записываем BOM для Excel и заголовки таблицы
    csv_file << "\xEF\xBB\xBF";
    csv_file << "Название;Цена;Имя файла обложки\n";

    for(const auto& book: results)
        csv_file << book.title << ";" << book.price << ";" << book.img_filename << "\n";

    return 0;
}

