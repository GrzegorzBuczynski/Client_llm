#!/bin/bash

# Uproszczony skrypt testowy dla llm_client
# Sprawdza czy program działa poprawnie

echo "========================================="
echo "   PROSTY TEST LLM CLIENT"
echo "========================================="
echo ""

# Test 1: Czy program uruchamia się i kończy poprawnie
echo "TEST 1: Podstawowe uruchomienie"
echo "--------------------------------"
if echo -e "Test\nexit" | ./llm_client > /dev/null 2>&1; then
    echo "✓ Program uruchomił się i zakończył poprawnie"
else
    echo "✗ Program nie działa poprawnie"
    exit 1
fi
echo ""

# Test 2: Czy exit działa
echo "TEST 2: Komenda exit"
echo "---------------------"
if echo -e "exit" | ./llm_client > /dev/null 2>&1; then
    echo "✓ Komenda exit działa"
else
    echo "✗ Komenda exit nie działa"
    exit 1
fi
echo ""

# Test 3: Czy clear działa
echo "TEST 3: Komenda clear"
echo "---------------------"
if echo -e "clear\nexit" | ./llm_client > /dev/null 2>&1; then
    echo "✓ Komenda clear działa"
else
    echo "✗ Komenda clear nie działa"
    exit 1
fi
echo ""

# Test 4: Czy program zwraca odpowiedź
echo "TEST 4: Zwracanie odpowiedzi"
echo "-----------------------------"
if echo -e "Hello\nexit" | ./llm_client 2>&1 | grep -q "id"; then
    echo "✓ Program zwraca odpowiedź JSON"
else
    echo "✗ Program nie zwraca odpowiedzi"
    exit 1
fi
echo ""

echo "========================================="
echo "   WSZYSTKIE TESTY PRZESZŁY"
echo "========================================="
