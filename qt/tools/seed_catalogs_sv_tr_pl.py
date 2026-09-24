#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Seed the Swedish, Turkish and Polish catalogs.

Same vocabulary and the same rules as tools/seed_wave1_catalogs.py; kept in a
second file only so that map did not have to be rewritten wholesale. Both
should be folded into one table when the translation platform takes over, at
which point these scripts stop being the source of truth.

Per-language loanword judgment (plan rule 6), not a blanket policy:
  * Swedish keeps Mining, Pool, Peers and Escrow, which its speakers use.
  * Turkish localizes nearly everything: Madencilik, Havuz, Eşler, Emanet.
  * Polish keeps Pool and Escrow but localizes mining as Kopanie.
UTXO and Covenants are universal everywhere and are written as themselves.
"""

import io
import os
import re
import sys

T = {
    # navigation
    "Overview": {"sv": u"Översikt", "tr": u"Genel Bakış", "pl": u"Przegląd"},
    "Wallet": {"sv": u"Plånbok", "tr": u"Cüzdan", "pl": u"Portfel"},
    "Send": {"sv": u"Skicka", "tr": u"Gönder", "pl": u"Wyślij"},
    "Receive": {"sv": u"Ta emot", "tr": u"Al", "pl": u"Odbierz"},
    "Transactions": {"sv": u"Transaktioner", "tr": u"İşlemler", "pl": u"Transakcje"},
    "Pay/Collect": {"sv": u"Betala/Inkassera", "tr": u"Öde/Tahsil Et", "pl": u"Zapłać/Odbierz"},
    "Pool": {"sv": u"Pool", "tr": u"Havuz", "pl": u"Pool"},
    "Shielded": {"sv": u"Skyddad", "tr": u"Korumalı", "pl": u"Chronione"},
    "Mining": {"sv": u"Mining", "tr": u"Madencilik", "pl": u"Kopanie"},
    "Settings": {"sv": u"Inställningar", "tr": u"Ayarlar", "pl": u"Ustawienia"},
    "Hardware Wallet": {"sv": u"Hårdvaruplånbok", "tr": u"Donanım Cüzdanı", "pl": u"Portfel sprzętowy"},
    "Payments": {"sv": u"Betalningar", "tr": u"Ödemeler", "pl": u"Płatności"},
    "Escrow": {"sv": u"Escrow", "tr": u"Emanet", "pl": u"Escrow"},
    "Marketplace": {"sv": u"Marknadsplats", "tr": u"Pazar Yeri", "pl": u"Rynek"},
    "Peers": {"sv": u"Peers", "tr": u"Eşler", "pl": u"Peery"},
    "Template": {"sv": u"Mall", "tr": u"Şablon", "pl": u"Szablon"},
    "Liquidity Vault": {"sv": u"Likviditetsvalv", "tr": u"Likidite Kasası", "pl": u"Skarbiec płynności"},
    "Utreexo Proofs": {"sv": u"Utreexo-bevis", "tr": u"Utreexo Kanıtları", "pl": u"Dowody Utreexo"},
    # actions
    "Cancel": {"sv": u"Avbryt", "tr": u"İptal", "pl": u"Anuluj"},
    "Close": {"sv": u"Stäng", "tr": u"Kapat", "pl": u"Zamknij"},
    "Copy": {"sv": u"Kopiera", "tr": u"Kopyala", "pl": u"Kopiuj"},
    "Refresh": {"sv": u"Uppdatera", "tr": u"Yenile", "pl": u"Odśwież"},
    "Clear": {"sv": u"Rensa", "tr": u"Temizle", "pl": u"Wyczyść"},
    "Stop": {"sv": u"Stoppa", "tr": u"Durdur", "pl": u"Zatrzymaj"},
    "Confirm": {"sv": u"Bekräfta", "tr": u"Onayla", "pl": u"Potwierdź"},
    "Continue": {"sv": u"Fortsätt", "tr": u"Devam", "pl": u"Kontynuuj"},
    "Search": {"sv": u"Sök", "tr": u"Ara", "pl": u"Szukaj"},
    "Details": {"sv": u"Detaljer", "tr": u"Ayrıntılar", "pl": u"Szczegóły"},
    "Help": {"sv": u"Hjälp", "tr": u"Yardım", "pl": u"Pomoc"},
    # labels
    "Amount:": {"sv": u"Belopp:", "tr": u"Tutar:", "pl": u"Kwota:"},
    "Amount": {"sv": u"Belopp", "tr": u"Tutar", "pl": u"Kwota"},
    "Amount (DIN):": {"sv": u"Belopp (DIN):", "tr": u"Tutar (DIN):", "pl": u"Kwota (DIN):"},
    "From:": {"sv": u"Från:", "tr": u"Kimden:", "pl": u"Od:"},
    "To:": {"sv": u"Till:", "tr": u"Kime:", "pl": u"Do:"},
    "Address:": {"sv": u"Adress:", "tr": u"Adres:", "pl": u"Adres:"},
    "Address": {"sv": u"Adress", "tr": u"Adres", "pl": u"Adres"},
    "Balance:": {"sv": u"Saldo:", "tr": u"Bakiye:", "pl": u"Saldo:"},
    "Balance": {"sv": u"Saldo", "tr": u"Bakiye", "pl": u"Saldo"},
    "Fee:": {"sv": u"Avgift:", "tr": u"Ücret:", "pl": u"Opłata:"},
    "Fee": {"sv": u"Avgift", "tr": u"Ücret", "pl": u"Opłata"},
    "Status:": {"sv": u"Status:", "tr": u"Durum:", "pl": u"Status:"},
    "Status": {"sv": u"Status", "tr": u"Durum", "pl": u"Status"},
    "Destination:": {"sv": u"Mål:", "tr": u"Hedef:", "pl": u"Cel:"},
    "Provider:": {"sv": u"Leverantör:", "tr": u"Sağlayıcı:", "pl": u"Dostawca:"},
    "Password:": {"sv": u"Lösenord:", "tr": u"Parola:", "pl": u"Hasło:"},
    "Password": {"sv": u"Lösenord", "tr": u"Parola", "pl": u"Hasło"},
    "Confirm Password:": {"sv": u"Bekräfta lösenord:", "tr": u"Parolayı onayla:", "pl": u"Potwierdź hasło:"},
    "Height:": {"sv": u"Höjd:", "tr": u"Yükseklik:", "pl": u"Wysokość:"},
    "Date:": {"sv": u"Datum:", "tr": u"Tarih:", "pl": u"Data:"},
    "Type:": {"sv": u"Typ:", "tr": u"Tür:", "pl": u"Typ:"},
    "Label:": {"sv": u"Etikett:", "tr": u"Etiket:", "pl": u"Etykieta:"},
    "Confirmations:": {"sv": u"Bekräftelser:", "tr": u"Onaylar:", "pl": u"Potwierdzenia:"},
    "Confirmations": {"sv": u"Bekräftelser", "tr": u"Onaylar", "pl": u"Potwierdzenia"},
    "Total:": {"sv": u"Totalt:", "tr": u"Toplam:", "pl": u"Razem:"},
    "Total": {"sv": u"Totalt", "tr": u"Toplam", "pl": u"Razem"},
    "Available:": {"sv": u"Tillgängligt:", "tr": u"Kullanılabilir:", "pl": u"Dostępne:"},
    "Available": {"sv": u"Tillgängligt", "tr": u"Kullanılabilir", "pl": u"Dostępne"},
    "Pending:": {"sv": u"Väntande:", "tr": u"Beklemede:", "pl": u"Oczekujące:"},
    "Pending": {"sv": u"Väntande", "tr": u"Beklemede", "pl": u"Oczekujące"},
    "Spendable:": {"sv": u"Spenderbart:", "tr": u"Harcanabilir:", "pl": u"Do wydania:"},
    "Immature:": {"sv": u"Omoget:", "tr": u"Olgunlaşmamış:", "pl": u"Niedojrzałe:"},
    "Unconfirmed:": {"sv": u"Obekräftat:", "tr": u"Onaylanmamış:", "pl": u"Niepotwierdzone:"},
    "Transaction ID:": {"sv": u"Transaktions-ID:", "tr": u"İşlem kimliği:", "pl": u"ID transakcji:"},
    "Recipient:": {"sv": u"Mottagare:", "tr": u"Alıcı:", "pl": u"Odbiorca:"},
    "Sender:": {"sv": u"Avsändare:", "tr": u"Gönderen:", "pl": u"Nadawca:"},
    "Invoice ID:": {"sv": u"Faktura-ID:", "tr": u"Fatura kimliği:", "pl": u"ID faktury:"},
    "Invoice Details": {"sv": u"Fakturadetaljer", "tr": u"Fatura ayrıntıları", "pl": u"Szczegóły faktury"},
    # states and messages
    "Connecting...": {"sv": u"Ansluter…", "tr": u"Bağlanıyor…", "pl": u"Łączenie…"},
    "Connected": {"sv": u"Ansluten", "tr": u"Bağlandı", "pl": u"Połączono"},
    "Error": {"sv": u"Fel", "tr": u"Hata", "pl": u"Błąd"},
    "Warning": {"sv": u"Varning", "tr": u"Uyarı", "pl": u"Ostrzeżenie"},
    "Invalid Input": {"sv": u"Ogiltig inmatning", "tr": u"Geçersiz giriş", "pl": u"Nieprawidłowe dane"},
    "Invalid Amount": {"sv": u"Ogiltigt belopp", "tr": u"Geçersiz tutar", "pl": u"Nieprawidłowa kwota"},
    "Invalid Address": {"sv": u"Ogiltig adress", "tr": u"Geçersiz adres", "pl": u"Nieprawidłowy adres"},
    "Input Required": {"sv": u"Inmatning krävs", "tr": u"Giriş gerekli", "pl": u"Wymagane dane"},
    "Please enter an amount.": {"sv": u"Ange ett belopp.", "tr": u"Lütfen bir tutar girin.", "pl": u"Podaj kwotę."},
    "Amount must be greater than 0.": {"sv": u"Beloppet måste vara större än 0.", "tr": u"Tutar 0'dan büyük olmalıdır.", "pl": u"Kwota musi być większa niż 0."},
    "Copied to clipboard": {"sv": u"Kopierat till urklipp", "tr": u"Panoya kopyalandı", "pl": u"Skopiowano do schowka"},
    "Copy Invoice": {"sv": u"Kopiera faktura", "tr": u"Faturayı kopyala", "pl": u"Kopiuj fakturę"},
    "Copy Package": {"sv": u"Kopiera paket", "tr": u"Paketi kopyala", "pl": u"Kopiuj pakiet"},
    "Conversion Successful": {"sv": u"Konvertering lyckades", "tr": u"Dönüştürme başarılı", "pl": u"Konwersja udana"},
    "Conversion Failed": {"sv": u"Konvertering misslyckades", "tr": u"Dönüştürme başarısız", "pl": u"Konwersja nie powiodła się"},
    "Enter amount to convert": {"sv": u"Ange belopp att konvertera", "tr": u"Dönüştürülecek tutarı girin", "pl": u"Podaj kwotę do konwersji"},
    "API Key": {"sv": u"API-nyckel", "tr": u"API anahtarı", "pl": u"Klucz API"},
    "AI Settings": {"sv": u"AI-inställningar", "tr": u"Yapay zekâ ayarları", "pl": u"Ustawienia AI"},
    # wallet flow
    "Wallet Name:": {"sv": u"Plånbokens namn:", "tr": u"Cüzdan adı:", "pl": u"Nazwa portfela:"},
    "Wallet Fingerprint:": {"sv": u"Plånbokens fingeravtryck:", "tr": u"Cüzdan parmak izi:", "pl": u"Odcisk portfela:"},
    "First Address:": {"sv": u"Första adressen:", "tr": u"İlk adres:", "pl": u"Pierwszy adres:"},
    "Creating wallet...": {"sv": u"Skapar plånbok…", "tr": u"Cüzdan oluşturuluyor…", "pl": u"Tworzenie portfela…"},
    "Wallet Setup Complete": {"sv": u"Installationen är klar", "tr": u"Kurulum tamamlandı", "pl": u"Konfiguracja zakończona"},
    "Your Dinero wallet is ready to use": {"sv": u"Din Dinero-plånbok är redo att användas", "tr": u"Dinero cüzdanınız kullanıma hazır", "pl": u"Twój portfel Dinero jest gotowy"},
    "Your Seed Phrase": {"sv": u"Din seed-fras", "tr": u"Seed ifadeniz", "pl": u"Twoja fraza seed"},
    "Confirm Your Seed Phrase": {"sv": u"Bekräfta din seed-fras", "tr": u"Seed ifadenizi onaylayın", "pl": u"Potwierdź frazę seed"},
    "Enter word...": {"sv": u"Ange ord…", "tr": u"Kelimeyi girin…", "pl": u"Wpisz słowo…"},
    "Incorrect Words": {"sv": u"Felaktiga ord", "tr": u"Yanlış kelimeler", "pl": u"Nieprawidłowe słowa"},
    "Backup Required": {"sv": u"Säkerhetskopia krävs", "tr": u"Yedek gerekli", "pl": u"Wymagana kopia zapasowa"},
    "Import Required": {"sv": u"Import krävs", "tr": u"İçe aktarma gerekli", "pl": u"Wymagany import"},
    "Import Complete": {"sv": u"Importen är klar", "tr": u"İçe aktarma tamamlandı", "pl": u"Import zakończony"},
    # language picker
    "Language": {"sv": u"Språk", "tr": u"Dil", "pl": u"Język"},
    "Interface language:": {"sv": u"Gränssnittsspråk:", "tr": u"Arayüz dili:", "pl": u"Język interfejsu:"},
    "Restart Dinero Now": {"sv": u"Starta om Dinero nu", "tr": u"Dinero'yu şimdi yeniden başlat", "pl": u"Uruchom Dinero ponownie"},
    "Restart Now": {"sv": u"Starta om nu", "tr": u"Şimdi yeniden başlat", "pl": u"Uruchom ponownie"},
    "Later": {"sv": u"Senare", "tr": u"Daha sonra", "pl": u"Później"},
    "Restart Dinero": {"sv": u"Starta om Dinero", "tr": u"Dinero'yu yeniden başlat", "pl": u"Uruchom Dinero ponownie"},
    "Language saved. Restart Dinero to apply it.": {
        "sv": u"Språket har sparats. Starta om Dinero för att tillämpa det.",
        "tr": u"Dil kaydedildi. Uygulamak için Dinero'yu yeniden başlatın.",
        "pl": u"Język zapisany. Uruchom Dinero ponownie, aby go zastosować."},
    "Show Developer Menu": {"sv": u"Visa utvecklarmeny", "tr": u"Geliştirici menüsünü göster", "pl": u"Pokaż menu dewelopera"},
    "Hide Developer Menu": {"sv": u"Dölj utvecklarmeny", "tr": u"Geliştirici menüsünü gizle", "pl": u"Ukryj menu dewelopera"},
}

UNIVERSAL = {"Covenants": u"Covenants", "UTXOs": u"UTXO"}


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def seed(path, lang):
    text = io.open(path, encoding="utf-8").read()
    applied = 0
    pairs = [(s, m[lang]) for s, m in T.items() if lang in m] + list(UNIVERSAL.items())
    for source, target in pairs:
        pattern = re.compile(
            r"(<source>" + re.escape(esc(source)) +
            r"</source>\s*)<translation type=\"unfinished\"></translation>")
        text, n = pattern.subn(
            lambda m: m.group(1) + "<translation>" + esc(target) + "</translation>",
            text, count=1)
        applied += n
    io.open(path, "w", encoding="utf-8").write(text)
    return applied


def main():
    base = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "translations")
    for lang in ("sv", "tr", "pl"):
        path = os.path.normpath(os.path.join(base, "dinero_%s.ts" % lang))
        if not os.path.exists(path):
            print("missing catalog:", path)
            return 1
        print("%-4s seeded %d entries" % (lang, seed(path, lang)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
