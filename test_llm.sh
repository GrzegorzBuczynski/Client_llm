#!/bin/bash

# Skrypt testowy dla llm_client
# Testuje różne scenariusze konwersacji z LLM

echo "========================================="
echo "   TEST LLM CLIENT"
echo "========================================="
echo ""

# Funkcja: wyślij prompt natychmiast i czekaj maks. 120s na odpowiedź
run_prompt_with_timeout() {
    local prompt="$1"
    echo "(wysyłam prompt natychmiast, czekam maks. 120s na odpowiedź)"
    coproc CLIENT ( ./llm_client )

    # Wyślij prompt od razu
    echo "Ty: $prompt"
    echo "$prompt" >&"${CLIENT[1]}"

    local start_ts=$(date +%s)
    local saw_ai=0

    # Czytaj wyjście programu; zakończ gdy pojawi się ponowna zachęta "Ty:" po odpowiedzi AI
    while true; do
        if read -t 1 -u "${CLIENT[0]}" line; then
            echo "$line"
            if [[ "$line" == AI:* ]]; then
                saw_ai=1
            fi
            if [[ $saw_ai -eq 1 && "$line" == Ty:* ]]; then
                echo "exit" >&"${CLIENT[1]}"
                break
            fi
        else
            now_ts=$(date +%s)
            if (( now_ts - start_ts >= 120 )); then
                echo "(przerwano po 120s bez pełnej odpowiedzi)"
                echo "exit" >&"${CLIENT[1]}"
                break
            fi
        fi
    done

    # Poczekaj aż proces się zakończy (ignoruj błędy jeśli już zakończony)
    wait "$COPROC_PID" 2>/dev/null || true
}

# Funkcja: utrzymaj jedną sesję i wyślij wiele promptów po kolei
run_session_with_prompts() {
    echo "(uruchamiam jedną sesję; kilka wymian w tej samej konwersacji)"
    coproc CLIENT ( ./llm_client )

    local start_ts
    local now_ts
    local line
    local prompt

    for prompt in "$@"; do
        echo "Ty: $prompt"
        echo "$prompt" >&"${CLIENT[1]}"

        start_ts=$(date +%s)
        local saw_ai=0

        while true; do
            if read -t 1 -u "${CLIENT[0]}" line; then
                echo "$line"
                if [[ "$line" == AI:* ]]; then
                    saw_ai=1
                fi
                if [[ $saw_ai -eq 1 && "$line" == Ty:* ]]; then
                    break
                fi
            else
                now_ts=$(date +%s)
                if (( now_ts - start_ts >= 120 )); then
                    echo "(przerwano po 120s bez pełnej odpowiedzi)"
                    break
                fi
            fi
        done
    done

    # Zakończ sesję
    echo "exit" >&"${CLIENT[1]}"
    wait "$COPROC_PID" 2>/dev/null || true
}

# Test 1: Prosta konwersacja
echo "TEST 1: Prosta konwersacja"
echo "-------------------------"
echo "Testuję podstawową konwersację..."
run_prompt_with_timeout "Cześć!"
echo ""
echo "✓ Test 1 zakończony"
echo ""
echo "Czekam 10 sekund przed następnym testem..."
sleep 10

# Test 2: Konwersacja z kontekstem
echo "TEST 2: Konwersacja z kontekstem"
echo "--------------------------------"
echo "Testuję pamięć kontekstu..."
run_session_with_prompts "Nazywam się Jan" "Jak się czujesz?"
echo ""
echo "✓ Test 2 zakończony"
echo ""
echo "Czekam 10 sekund przed następnym testem..."
sleep 10

# Test 3: Komenda clear
echo "TEST 3: Komenda clear"
echo "---------------------"
echo "Testuję czyszczenie konwersacji..."
run_session_with_prompts "Cześć!" "Jak się masz?" "clear" "Witam ponownie"
echo ""
echo "✓ Test 3 zakończony"
echo ""
echo "Czekam 10 sekund przed następnym testem..."
sleep 10

# Test 4: Puste wejście
echo "TEST 4: Puste wejście"
echo "---------------------"
echo "Testuję obsługę pustego wejścia..."
run_prompt_with_timeout ""
run_prompt_with_timeout "Cześć!"
echo ""
echo "✓ Test 4 zakończony"
echo ""
echo "Czekam 10 sekund przed następnym testem..."
sleep 10

# Test 5: Znaki specjalne
echo "TEST 5: Znaki specjalne"
echo "-----------------------"
echo "Testuję znaki specjalne..."
run_prompt_with_timeout "Czy \"to\" działa?"
echo ""
echo "✓ Test 5 zakończony"
echo ""

echo "========================================="
echo "   WSZYSTKIE TESTY ZAKOŃCZONE"
echo "========================================="
