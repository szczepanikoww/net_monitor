# Network Service Monitor via SCTP (Project 5)

Ten projekt jest programem typu "Demon", który w tle monitoruje dostępność podanych serwerów w sieci za pomocą skanowania portów TCP (80 i 443). Pozwala również na dynamiczne zarządzanie listą obserwowanych serwerów bez jego wyłączania – służy do tego dedykowana aplikacja kliencka, która komunikuje się z Demonem po protokole **SCTP**.

Projekt ten łączy wiedzę z zajęć (Syslog, Demony, Rozwiązywanie nazw IP, Sockets, SCTP). 

## Funkcjonalności

- **Monitoring wieloportowy** — demon sprawdza zarówno port **80** (HTTP) jak i **443** (HTTPS) na każdym hoście
- **Pomiar opóźnień (latency)** — mierzy czas nawiązywania połączenia TCP w milisekundach za pomocą `gettimeofday()`
- **Rozwiązywanie DNS z logowaniem** — loguje rozwiązane adresy IP za pomocą `getaddrinfo()` i `inet_ntop()`
- **Rozbudowane logowanie** — 8 kategorii logów (INIT, CLIENT, CYCLE, SCAN, DNS, PROBE, ALERT, RESULT) daje pełny wgląd w pracę demona
- **Komenda LOG** — klient może pobierać ostatnie N zdarzeń z pamięci demona bez potrzeby dostępu do sysloga
- **Detekcja zmian statusu** — demon generuje alerty (ALERT) z informacją o przejściu (np. `UNKNOWN -> UP`) gdy host zmienia stan
- **Ochrona przed duplikatami** — komenda ADD zwraca błąd, gdy host już istnieje na liście
- **Komunikacja SCTP** — klient i demon komunikują się po protokole SCTP (port 9000)

## Kategorie Logów

Demon używa 8 prefiksów w logach systemowych, ułatwiających filtrowanie i analizę:

| Prefiks | Opis |
|---------|------|
| `INIT` | Etapy uruchamiania demona (socket, bind, listen) |
| `CLIENT` | Odebrane komendy od klienta z adresem IP i portem źródłowym |
| `CYCLE` | Początek i koniec cyklu monitorowania z podsumowaniem (X UP, Y DOWN) |
| `SCAN` | Ogłoszenie rozpoczęcia sprawdzania konkretnego hosta |
| `DNS` | Rozwiązywanie nazw domenowych z rozwiązanym adresem IP |
| `PROBE` | Wynik sondowania konkretnego portu (OPEN/CLOSED/TIMEOUT z czasem) |
| `ALERT` | Zmiana statusu hosta z informacją o przejściu (np. `DOWN -> UP`) |
| `RESULT` | Końcowy wynik sprawdzenia hosta w danym cyklu |

## Wymagania

Ponieważ używamy `syslog`, funkcji systemowych Linuxa oraz protokołu SCTP, kod musi być kompilowany na maszynie z systemem **Linux** (takiej jak uniwersytecki serwer "pluton" lub lokalny system Linux/Ubuntu). 

Zanim przystąpisz do kompilacji, upewnij się, że biblioteka `libsctp-dev` jest zainstalowana na Twoim systemie, co było wymogiem z LAB06:
```bash
sudo apt-get update
sudo apt-get install libsctp-dev
```

## Kompilacja

Projekt zawiera zautomatyzowany skrypt budujący. Aby skompilować program, wejdź w terminalu do folderu projektu i wpisz polecenie:
```bash
make
```

Pomyślna kompilacja utworzy dwa pliki wykonywalne:
1. `netmon_daemon` (Główny demon działający w tle)
2. `netmon_client` (Program dla administratora do wysyłania komend)

## Jak uruchomić i używać programu

1. **Uruchom Demona**
Uruchom w konsoli główny program:
```bash
./netmon_daemon
```
Program natychmiastowo zakończy swoje działanie w konsoli. Zgodnie ze sztuką systemów uniksowych odłączył się on od terminala i wszedł w tło (tzw. Demonizacja). Od tego momentu po cichu nasłuchuje Twoich poleceń na porcie SCTP (Port 9000).

2. **Sprawdź logi systemowe**
Aby zobaczyć logi z Demona, użyj komendy, która podgląda logi systemowe na żywo:
```bash
tail -f /var/log/syslog | grep netmon_daemon
```
Od razu po uruchomieniu zobaczysz pełny baner startowy:
```
netmon_daemon[1234]: ========================================
netmon_daemon[1234]: Network Monitor Daemon started (PID: 1234)
netmon_daemon[1234]: Max watchlist capacity: 50 hosts
netmon_daemon[1234]: Monitoring ports: 80 (HTTP), 443 (HTTPS)
netmon_daemon[1234]: Check interval: 5 seconds
netmon_daemon[1234]: ========================================
netmon_daemon[1234]: INIT: Creating SCTP socket (SOCK_SEQPACKET)...
netmon_daemon[1234]: INIT: SCTP socket created (fd=3)
netmon_daemon[1234]: INIT: Binding to 0.0.0.0:9000...
netmon_daemon[1234]: INIT: Bind successful
netmon_daemon[1234]: INIT: Listening on SCTP port 9000. Waiting for commands...
```
Po dodaniu hostów, co 5 sekund zobaczysz pełny cykl sprawdzania:
```
netmon_daemon[1234]: --- CYCLE #1: Checking 2 host(s) ---
netmon_daemon[1234]: SCAN: Starting check for google.com...
netmon_daemon[1234]: DNS: google.com resolved to 142.250.186.206
netmon_daemon[1234]: PROBE: google.com:80 is OPEN (12.3ms)
netmon_daemon[1234]: PROBE: google.com:443 is OPEN (11.8ms)
netmon_daemon[1234]: ALERT: Host google.com changed UNKNOWN -> UP [80,443] (11.8ms)
netmon_daemon[1234]: RESULT: google.com is UP [80,443] (11.8ms)
netmon_daemon[1234]: SCAN: Starting check for 192.168.1.1...
netmon_daemon[1234]: DNS: 192.168.1.1 resolved to 192.168.1.1
netmon_daemon[1234]: PROBE: 192.168.1.1:80 TIMEOUT (no response in 1000ms)
netmon_daemon[1234]: PROBE: 192.168.1.1:443 TIMEOUT (no response in 1000ms)
netmon_daemon[1234]: ALERT: Host 192.168.1.1 changed UNKNOWN -> DOWN [-]
netmon_daemon[1234]: RESULT: 192.168.1.1 is DOWN [-]
netmon_daemon[1234]: --- CYCLE #1 COMPLETE: 1 UP, 1 DOWN ---
```
(Powyższą komendę warto mieć włączoną na osobnym terminalu, by móc obserwować poczynania programu).

3. **Dodaj serwery do monitorowania używając Klienta**
Użyj klienta, aby wysłać do Demona komendę `ADD` połączoną z adresem serwera.
```bash
./netmon_client ADD google.com
./netmon_client ADD wp.pl
./netmon_client ADD 192.168.1.1
```
W syslogu zobaczysz:
```
netmon_daemon[1234]: CLIENT: Received command "ADD google.com" from 127.0.0.1:48372
netmon_daemon[1234]: Added host google.com to watchlist
```
Próba dodania hosta, który już jest na liście, zwróci błąd: `ERROR: Host google.com already on watchlist`.

4. **Sprawdź aktualny status (Dashboard)**
W każdej chwili możesz zapytać Demona o to, jaki jest stan wszystkich obserwowanych serwerów, używając polecenia `STATUS`:
```bash
./netmon_client STATUS
```
Program zwróci na terminal sformatowaną listę adresów z portami i opóźnieniem:
```
Watchlist Status:
- google.com: UP [80,443] (11.8ms)
- wp.pl: UP [80,443] (45.7ms)
- 192.168.1.1: DOWN [-]
```

5. **Podejrzyj historię zdarzeń (Event Log)**
Zamiast wchodzić do sysloga, możesz poprosić demona o ostatnie zdarzenia:
```bash
./netmon_client LOG        # domyślnie ostatnie 10 zdarzeń
./netmon_client LOG 20     # ostatnie 20 zdarzeń
```
Przykładowy wynik:
```
Event Log:
  [14:23:01] CLIENT: Received command "ADD google.com" from 127.0.0.1:48372
  [14:23:01] Added host google.com to watchlist
  [14:23:05] --- CYCLE #1: Checking 1 host(s) ---
  [14:23:05] SCAN: Starting check for google.com...
  [14:23:05] DNS: google.com resolved to 142.250.186.206
  [14:23:05] PROBE: google.com:80 is OPEN (12.3ms)
  [14:23:05] PROBE: google.com:443 is OPEN (11.8ms)
  [14:23:05] ALERT: Host google.com changed UNKNOWN -> UP [80,443] (11.8ms)
  [14:23:05] RESULT: google.com is UP [80,443] (11.8ms)
  [14:23:05] --- CYCLE #1 COMPLETE: 1 UP, 0 DOWN ---
```

6. **Usuń serwery z listy obserwowanych**
```bash
./netmon_client REMOVE wp.pl
```

7. **Zakończ działanie Demona**
Gdy demon już nie jest potrzebny, można go zabić jak każdy zwyczajny proces w systemie Linux. Najpierw znajdujemy jego identyfikator (PID), a potem używamy `kill`:
```bash
pidof netmon_daemon
kill -9 <uzyskany_numer>
```
Można też po prostu zabić go na skróty komendą: `killall netmon_daemon`
