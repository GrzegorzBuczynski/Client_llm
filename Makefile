# Makefile dla programu klienta LLM używającego libcurl

# Nazwa pliku wykonywalnego
TARGET = llm_client
# Plik źródłowy
SRC = llm_client.cpp
# Kompilator C++
CXX = g++

# Flagi linkera: Bezpośrednie użycie flag libcurl
# Używa standardowych ścieżek systemowych
LDFLAGS = -lcurl
INCLUDE_FLAGS = -I/usr/include/x86_64-linux-gnu

# Flagi kompilatora: nowoczesny standard C++ i włączenie wszystkich ostrzeżeń
CXXFLAGS = -std=c++17 -Wall

# Cel domyślny: buduje plik wykonywalny
all: $(TARGET)

$(TARGET): $(SRC)
	@echo "Kompilowanie klienta LLM..."
	$(CXX) $(SRC) $(CXXFLAGS) $(INCLUDE_FLAGS) -o $(TARGET) $(LDFLAGS)

# Cel 'run': kompiluje i uruchamia program
run: all
	@echo "Uruchamianie programu..."
	./$(TARGET)

# Cel 'clean': usuwa skompilowany plik
clean:
	@echo "Czyszczenie..."
	rm -f $(TARGET)

.PHONY: all clean run
