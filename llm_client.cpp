#include <iostream>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>

// --- Stałe konfiguracyjne ---
const std::string API_URL = "http://127.0.0.1:1234/v1/chat/completions";
// Domyślna nazwa modelu (można nadpisać w config.ini)
const std::string DEFAULT_MODEL_NAME = "qwen q2";


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

    // Używamy max_tokens podobnego do przykładu użytkownika i temperatury z parametru
    json_payload << "\"temperature\": " << temperature << ",";
    json_payload << "\"max_tokens\": 1024";

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

int main() {
    std::cout << "--- Klient LLM z Konwersacją ---\n";
    std::cout << "Wpisz 'exit' żeby zakończyć, 'clear' żeby wyczyścić konwersację\n\n";

    std::vector<ChatMessage> conversation_history;

    // --- Wczytaj konfigurację z pliku config.ini (jeśli istnieje) ---
    std::string model_name = DEFAULT_MODEL_NAME;
    bool include_history = true; // domyślnie dołączamy historię

    // Domyślna rola dla wpisu użytkownika (można nadpisać w config.ini)
    std::string role = "user";
    bool role_was_prompt = false;
    if (parse_config("config.ini", model_name, include_history, role, role_was_prompt)) {
        if (role_was_prompt) {
            std::cout << "[CONFIG] model=" << model_name << ", include_history=" << (include_history?"true":"false") << ", ROLE looked like a prompt so SYSTEM_PROMPT was set; sending role=user\n";
        } else {
            std::cout << "[CONFIG] model=" << model_name << ", include_history=" << (include_history?"true":"false") << ", role=" << role << "\n";
        }
    } else {
        std::cout << "[CONFIG] Nie znaleziono config.ini, używam domyślnych ustawień. model=" << model_name << ", role=" << role << "\n";
    }

    while (true) {
        std::cout << "Ty: ";
        std::string user_input;
        std::getline(std::cin, user_input);

        if (user_input == "exit") {
            std::cout << "Do widzenia!\n";
            break;
        }

        if (user_input == "clear") {
            conversation_history.clear();
            std::cout << "Konwersacja wyczyszczona.\n\n";
            continue;
        }

        if (user_input.empty()) {
            continue;
        }

            // Przygotuj wiadomości do wysłania
            std::vector<ChatMessage> messages_to_send;
            // pierwsza wiadomość to zawsze system prompt (może być nadpisany z config.ini)
            messages_to_send.push_back({ "system", SYSTEM_PROMPT });
            if (include_history) {
                for(const auto& msg : conversation_history) {
                    // Waliduj role w historii — jeśli jakaś jest niepoprawna, mapuj na "user"
                    std::string r = msg.role;
                    if (!is_valid_role(r)) {
                        std::cerr << "[WARN] Nieprawidłowa rola w historii: '" << r << "' — używam 'user' zamiast tego.\n";
                        r = "user";
                    }
                    messages_to_send.push_back({ r, msg.content });
                }
            }
            // Waliduj role wybrane w konfiguracji dla bieżącego inputu
            std::string send_role = role;
            if (!is_valid_role(send_role)) {
                // Jeśli wartość z ROLE jest niepoprawna, to jeśli już potraktowaliśmy ją jako SYSTEM_PROMPT
                // (role_was_prompt==true) nie pokazujemy warning przy każdym zapytaniu — tylko stosujemy fallback.
                if (!role_was_prompt) {
                    std::cerr << "[WARN] Wartość ROLE=('" << role << "') nie jest jedną z [user,assistant,system,tool]. "
                              << "Używam domyślnej roli 'user' dla wpisu.\n";
                }
                send_role = "user";
            }
            messages_to_send.push_back({ send_role, user_input });


    // Zbuduj i wyślij zapytanie (użyj model_name i role z config.ini lub domyślnych)
    std::string json_payload = build_chat_json_payload2(messages_to_send, 0.7, model_name, role);
        // std::cout << "[DEBUG] JSON Payload: " << json_payload << std::endl; // Odkomentuj do debugowania
        
        std::cout << "AI: " << std::flush;
        std::string response_json = execute_http_post(API_URL, json_payload);
        std::string response_text = parse_json_response(response_json);

        // Dodano sprawdzenie, czy odpowiedź została poprawnie sparsowana.
        // Jeśli nie, pętla jest kontynuowana bez dodawania błędnych danych do historii.
        if (response_text.empty()) {
            std::cout << "Błąd: Nie otrzymano poprawnej odpowiedzi od serwera.\n\n";
            continue;
        }

        std::cout << response_text << "\n\n";

        // Zapisz do historii tylko udaną wymianę
        conversation_history.push_back({ "user", user_input });
        conversation_history.push_back({ "assistant", response_text });
    }

    return 0;
}
