#include <iostream>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>

// --- Stałe konfiguracyjne ---
const std::string DEFAULT_API_URL = "http://127.0.0.1:1234/v1/chat/completions";
// API_URL może być nadpisany przez config.ini
std::string API_URL = DEFAULT_API_URL;
// Domyślna nazwa modelu (można nadpisać w config.ini)
const std::string DEFAULT_MODEL_NAME = "qwen q2";
// Limit słów w odpowiedzi (0 = bez limitu). Definiowane w kodzie lub przez CLI.
int MAX_WORDS = 30; // Zmienna X — ustaw tu preferowany limit słów lub nadpisz flagą --max-words


// Domyślny system prompt — można nadpisać w config.ini (klucz SYSTEM_PROMPT)
std::string SYSTEM_PROMPT = "You are a helpful AI assistant. Please answer concisely.";
// Struktura do przechowywania pojedynczej wiadomości w konwersacji
struct ChatMessage {
    std::string role;
    std::string content;
};

// Funkcja zwrotna (callback) do przechwytywania danych z odpowiedzi serwera.
size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Funkcja do parsowania odpowiedzi JSON i wyciągnięcia tekstu odpowiedzi
// Została zmodyfikowana, aby zwracać pusty string w przypadku błędu,
// co zapobiega dodawaniu błędnych danych do historii konwersacji.
std::string parse_json_response(const std::string& json_response) {
    std::string key = "\"content\": \"";
    size_t content_pos = json_response.rfind(key);
    if (content_pos == std::string::npos) {
        if (json_response.find("error") != std::string::npos) {
            std::cerr << "\n[DEBUG] Otrzymano błąd od serwera: " << json_response << std::endl;
        } else {
            std::cerr << "\n[DEBUG] Nie udało się wyodrębnić pola 'content' z JSON. Surowa odpowiedź: " << json_response << std::endl;
        }
        return ""; // Zwróć pusty string w przypadku błędu
    }

    size_t start_pos = content_pos + key.length();
    size_t end_pos = start_pos;
    bool escaped = false;
    while (end_pos < json_response.length()) {
        if (escaped) {
            escaped = false;
        } else if (json_response[end_pos] == '\\') {
            escaped = true;
        } else if (json_response[end_pos] == '"' && !escaped) {
            break;
        }
        end_pos++;
    }

    if (end_pos <= start_pos) {
        std::cerr << "\n[DEBUG] Nie znaleziono cudzysłowu zamykającego dla pola 'content'." << std::endl;
        return "";
    }

    std::string extracted = json_response.substr(start_pos, end_pos - start_pos);

    // Unescape podstawowych sekwencji
    auto unescape = [&](const std::string& find, const std::string& replace) {
        size_t pos = 0;
        while ((pos = extracted.find(find, pos)) != std::string::npos) {
            extracted.replace(pos, find.length(), replace);
            pos += replace.length();
        }
    };

    unescape("\\n", "\n");
    unescape("\\\"", "\"");
    unescape("\\\\", "\\");

    return extracted;
}

// Funkcja do escapowania znaków specjalnych w stringu dla JSON
std::string escape_json_string(const std::string& str) {
    std::string escaped;
    escaped.reserve(str.length());
    for (char c : str) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c >= 0 && c < 32) {
                    // Pozostałe znaki kontrolne, można je pominąć lub zamienić na unicode
                } else {
                    escaped += c;
                }
                break;
        }
    }
    return escaped;
}

// Funkcja do budowy JSON payload dla API /v1/chat/completions
std::string build_chat_json_payload(const std::vector<ChatMessage>& messages, float temperature, const std::string& model_name) {
    std::ostringstream json_payload;
    json_payload << "{"
                 << "\"model\": \"" << model_name << "\","
                 << "\"messages\": [";

    for (size_t i = 0; i < messages.size(); ++i) {
        json_payload << "{"
                     << "\"role\": \"" << messages[i].role << "\","
                     << "\"content\": \"" << escape_json_string(messages[i].content) << "\""
                     << "}";
        if (i < messages.size() - 1) {
            json_payload << ",";
        }
    }

    json_payload << "],"
                 << "\"max_tokens\": 150,"
                 << "\"temperature\": " << temperature << ","
                 << "\"repeat_penalty\": 1.2,"
                 << "\"stream\": false"
                 << "}";

    return json_payload.str();
}

std::string build_chat_json_payload2(const std::vector<ChatMessage>& messages, float temperature, const std::string& model_name, std::string& role) 
{
    std::ostringstream json_payload;
    json_payload << "{"
                 << "\"model\": \"" << model_name << "\",";

    json_payload << "\"messages\": [";
    for (size_t i = 0; i < messages.size(); ++i) {
        json_payload << "{"
                     << "\"role\": \"" << messages[i].role << "\"," 
                     << "\"content\": \"" << escape_json_string(messages[i].content) << "\""
                     << "}";
        if (i < messages.size() - 1) json_payload << ",";
    }
    json_payload << "],";

    // Używamy max_tokens zależnego od MAX_WORDS (przybliżenie: ~2 tokeny na słowo)
    int max_tokens = 1024;
    if (MAX_WORDS > 0) {
        max_tokens = std::max(16, MAX_WORDS * 2);
    }
    json_payload << "\"temperature\": " << temperature << ",";
    json_payload << "\"max_tokens\": " << max_tokens;

    json_payload << "}";
    return json_payload.str();
}

// Sprawdza, czy podana rola jest akceptowana przez serwer
bool is_valid_role(const std::string &r) {
    if (r == "user" || r == "assistant" || r == "system" || r == "tool") return true;
    return false;
}

// --- Parsowanie pliku config.ini ---
// Zwraca true jeśli plik został wczytany, a wartości model_name/include_history są ustawione.
// Rozszerzona funkcja: parsuje model, include_history oraz opcjonalnie ROLE
// role_was_prompt - out param: true if a non-role ROLE value was interpreted as SYSTEM_PROMPT
bool parse_config(const std::string& path, std::string &model_name, bool &include_history, std::string &role, bool &role_was_prompt) {
    std::ifstream cfg(path);
    if (!cfg) return false;

    auto ltrim = [](std::string &s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
    };
    auto rtrim = [](std::string &s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
    };

    std::string line;
    while (std::getline(cfg, line)) {
        ltrim(line); rtrim(line);
        if (line.empty()) continue;
        // Jeśli linia zaczyna się od ';' lub '#', traktujemy ją jako komentarz i pomijamy
        if (line[0] == ';' || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        ltrim(key); rtrim(key);
        ltrim(value); rtrim(value);

        if (!value.empty() && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        if (key == "MODEL_NAME") {
            if (!value.empty()) model_name = value;
        } else if (key == "INCLUDE_HISTORY") {
            std::string v = value;
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return std::tolower(c); });
            include_history = (v == "true" || v == "1" || v == "yes" || v == "y");
        } else if (key == "ROLE") {
            if (!value.empty()) {
                if (is_valid_role(value)) {
                    role = value;
                    role_was_prompt = false;
                } else {
                    // Treat as system prompt (user likely put prompt under ROLE)
                    SYSTEM_PROMPT = value;
                    role = "user"; // fallback role for sending
                    role_was_prompt = true;
                }
            }
        } else if (key == "SYSTEM_PROMPT") {
            if (!value.empty()) SYSTEM_PROMPT = value;
        } else if (key == "API_URL") {
            if (!value.empty()) API_URL = value;
        }
    }

    cfg.close();
    return true;
}

// Funkcja do wykonania zapytania HTTP POST przez CURL
std::string execute_http_post(const std::string& url, const std::string& json_data) {
    CURL *curl;
    CURLcode res;
    std::string response_buffer;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_data.length());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            response_buffer = "Błąd CURL: " + std::string(curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return response_buffer;
}

// Forward declarations for refactored helper functions
void init_config(std::string &model_name, bool &include_history, std::string &role, bool &role_was_prompt);
// NOTE: conversation_history removed — we do NOT send history to the model anymore
std::vector<ChatMessage> prepare_messages(const std::string &system_prompt,  const std::string &role, const std::string &user_input, bool role_was_prompt);
std::string send_and_receive(const std::vector<ChatMessage> &messages, const std::string &model_name, float temperature);
void save_exchange(std::vector<ChatMessage> &conversation_history, const std::string &user_input, const std::string &response_text);
void repl_loop(std::vector<ChatMessage> &conversation_history, const std::string &model_name, bool include_history, const std::string &role, bool role_was_prompt);
void parse_cli_args(int argc, char** argv);

// Inicjalizuje konfigurację (wywołuje parse_config i wypisuje skrócony komunikat)
void init_config(std::string &model_name, bool &include_history, std::string &role, bool &role_was_prompt) {
    role = "user";
    role_was_prompt = false;
    if (parse_config("config.ini", model_name, include_history, role, role_was_prompt)) {
        std::cout << "[CONFIG] model=" << model_name
                  << ", include_history=" << (include_history?"true":"false")
                  << ", api_url=" << API_URL
                  << ", max_words=" << (MAX_WORDS > 0 ? std::to_string(MAX_WORDS) : std::string("unlimited"))
                  << (role_was_prompt ? ", role=user (ROLE looked like prompt, set as SYSTEM_PROMPT)\n" : (std::string(", role=") + role + "\n"));
    } else {
    std::cout << "[CONFIG] Nie znaleziono config.ini, używam domyślnych ustawień. model=" << model_name
          << ", api_url=" << API_URL
          << ", role=" << role << "\n";
    }
}

// Przygotowuje wiadomości do wysłania (waliduje role). Historia konwersacji NIE JEST wysyłana do modelu.
std::vector<ChatMessage> prepare_messages(const std::string &system_prompt, const std::string &role, const std::string &user_input, bool role_was_prompt) {
    std::vector<ChatMessage> messages_to_send;
    messages_to_send.push_back({ "system", system_prompt });
    if (MAX_WORDS > 0) {
        messages_to_send.push_back({ "system", std::string("Please answer in no more than ") + std::to_string(MAX_WORDS) + " words." });
    }


    std::string send_role = role;
    if (!is_valid_role(send_role)) {
        if (!role_was_prompt) {
            std::cerr << "[WARN] Wartość ROLE=('" << role << "') nie jest jedną z [user,assistant,system,tool]. Używam domyślnej roli 'user' dla wpisu.\n";
        }
        send_role = "user";
    }

    messages_to_send.push_back({ send_role, user_input });
    return messages_to_send;
}

// Wysyła zapytanie i zwraca sparsowaną odpowiedź tekstową (albo pusty string przy błędzie)
std::string send_and_receive(const std::vector<ChatMessage> &messages, const std::string &model_name, float temperature) {
    std::string json_payload = build_chat_json_payload2(messages, temperature, model_name, const_cast<std::string&>(messages.back().role));
    std::string response_json = execute_http_post(API_URL, json_payload);
    std::string response_text = parse_json_response(response_json);
    return response_text;
}

// Zapisuje wymianę do historii
void save_exchange(std::vector<ChatMessage> &conversation_history, const std::string &user_input, const std::string &response_text) {
    conversation_history.push_back({ "user", user_input });
    conversation_history.push_back({ "assistant", response_text });
}

// Główna pętla REPL
void repl_loop(std::vector<ChatMessage> &conversation_history, const std::string &model_name, bool include_history, const std::string &role, bool role_was_prompt) {
    while (true) {
        std::cout << "Ty: ";
        std::string user_input;
        std::getline(std::cin, user_input);
        
        if (user_input == "exit") {
            std::cout << "Do widzenia!\n";
            break;
        }

        // Interaktywne ustawianie limitu (opcjonalne): setmax N
        if (user_input.rfind("setmax ", 0) == 0) {
            std::string n = user_input.substr(7);
            try {
                int v = std::stoi(n);
                MAX_WORDS = v >= 0 ? v : 0;
                std::cout << "[INFO] Ustawiono MAX_WORDS=" << MAX_WORDS << "\n\n";
            } catch (...) {
                std::cout << "[WARN] Niepoprawna liczba. Użycie: setmax N (N>=0).\n\n";
            }
            continue;
        }

        // Sprawdzenie aktualnego limitu słów: max / showmax
        if (user_input == "max" || user_input == "showmax") {
            if (MAX_WORDS > 0) {
                std::cout << "[INFO] Aktualny limit słów: " << MAX_WORDS << "\n\n";
            } else {
                std::cout << "[INFO] Limit słów: wyłączony (bez limitu)\n\n";
            }
            continue;
        }

        if (user_input == "clear") {
            conversation_history.clear();
            std::cout << "Konwersacja wyczyszczona.\n\n";
            continue;
        }
        if (user_input == "help") {
            std::cout << "Dostępne komendy:\n"
                         "  exit          - zakończ program\n"
                         "  clear         - wyczyść historię konwersacji\n"
                         "  setmax N      - ustaw maksymalną liczbę słów w odpowiedzi (N>=0, 0 = bez limitu)\n"
                         "  max|showmax   - pokaż aktualny limit słów\n"
                         "  help          - wyświetl tę pomoc\n\n";
            continue;
        }
        
        if (user_input.empty()) continue;

    auto messages = prepare_messages(SYSTEM_PROMPT, role, user_input, role_was_prompt);
        std::cout << "AI: " << std::flush;
        std::string response_text = send_and_receive(messages, model_name, 0.7f);
        
        if (response_text.empty()) {
            std::cout << "Błąd: Nie otrzymano poprawnej odpowiedzi od serwera.\n\n";
            continue;
        }
        
        std::cout << response_text << "\n\n";
        // save_exchange(conversation_history, user_input, response_text);
    }
}

int main(int argc, char** argv) {
    std::cout << "--- Klient LLM z Konwersacją ---\n";
    std::cout << "Wpisz 'exit' żeby zakończyć, 'clear' żeby wyczyścić konwersację\n\n";

    std::vector<ChatMessage> conversation_history;

    // --- Wczytaj konfigurację z pliku config.ini (jeśli istnieje) ---
    std::string model_name = DEFAULT_MODEL_NAME;
    bool include_history = false; // domyślnie nie dołączamy historii

    // CLI: obsłuż argumenty w dedykowanej funkcji
    parse_cli_args(argc, argv);

    // Refaktoryzacja: main teraz deleguje do małych funkcji
    // Inicjalizacja konfiguracji
    std::string role = "user";
    bool role_was_prompt = false;
    init_config(model_name, include_history, role, role_was_prompt);

    // REPL głównej pętli
    repl_loop(conversation_history, model_name, include_history, role, role_was_prompt);

    return 0;
}

// Proste parsowanie argumentów wiersza poleceń
void parse_cli_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--no-limit") {
            MAX_WORDS = 0;
        } else if (arg == "-w" || arg == "--max-words") {
            if (i + 1 < argc) {
                try {
                    int v = std::stoi(argv[++i]);
                    MAX_WORDS = v >= 0 ? v : 0;
                } catch (...) {
                    std::cerr << "[WARN] Niepoprawna wartość dla --max-words, ignoruję.\n";
                }
            } else {
                std::cerr << "[WARN] Brak wartości po --max-words. Użycie: --max-words N\n";
            }
        } else if (arg.rfind("--max-words=", 0) == 0) {
            std::string val = arg.substr(std::string("--max-words=").size());
            try {
                int v = std::stoi(val);
                MAX_WORDS = v >= 0 ? v : 0;
            } catch (...) {
                std::cerr << "[WARN] Niepoprawna wartość dla --max-words, ignoruję.\n";
            }
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Użycie: ./llm_client [opcje]\n"
                         "  -w, --max-words N     Ustaw maksymalną liczbę słów w odpowiedzi (N>=0)\n"
                         "      --max-words=N     Jak wyżej, forma z '='\n"
                         "      --no-limit        Wyłącz limit słów\n";
        }
    }
}
