#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>

// Конфигурация
#define ONE_WIRE_BUS 4
const char* ssid = "";
const char* password = "";
#define BOT_TOKEN ""
#define MASTER_PASSWORD ""
#define BOT_MTBS 500 // Интервал проверки сообщений (мс)

// Настройки времени (Москва GMT+3)
const char* ntpServer1 = "ru.pool.ntp.org";
const char* ntpServer2 = "europe.pool.ntp.org";
const long gmtOffset_sec = 10800;
const int daylightOffset_sec = 0;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress tempSensor;

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);
Preferences preferences;

// Настройки системы
bool reportEnabled = true;
int reportHour = 8;
float tempThreshold = 25.0;
const int MAX_USERS = 10;
String allowedChatIds[MAX_USERS];
int userCount = 0;
unsigned long bot_lasttime = 0;

void setup() {
  Serial.begin(115200);
  
  // Инициализация датчика (оптимизированная)
  sensors.begin();
  if (!sensors.getAddress(tempSensor, 0)) {
    Serial.println("Датчик DS18B20 не обнаружен!");
    while(1);
  }
  sensors.setResolution(10); // Оптимальное соотношение скорость/точность
  sensors.setWaitForConversion(false);
  
  // Настройка времени (российские серверы)
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
  
// Оптимизация подключения
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Отключаем энергосбережение WiFi
  
  // Установка корневого сертификата (обязательно)
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  client.setTimeout(5); // Таймаут соединения 5 сек

  // Загрузка настроек
  preferences.begin("temp-station", false);
  loadSettings();
  
  connectToWiFi();
}

void loadSettings() {
  reportEnabled = preferences.getBool("reportEnabled", true);
  reportHour = preferences.getInt("reportHour", 8);
  tempThreshold = preferences.getFloat("tempThreshold", 25.0);
  userCount = preferences.getInt("userCount", 0);
  
  for(int i = 0; i < userCount; i++) {
    allowedChatIds[i] = preferences.getString(("user_" + String(i)).c_str(), "");
  }
}

void loop() {
  static unsigned long lastTempRead = 0;
  static uint8_t lastDay = 0;
  
  // Асинхронное чтение температуры каждые 2 сек
  if (millis() - lastTempRead > 2000) {
    lastTempRead = millis();
    sensors.requestTemperatures();
  }

  // Оптимизированный опрос Telegram
  if (millis() - bot_lasttime > BOT_MTBS) {
    bot_lasttime = millis();
    handleTelegramMessages();
    checkSystem(&lastDay);
  }
}

void handleTelegramMessages() {
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  
  while(numNewMessages) {
    for (int i=0; i<numNewMessages; i++) {
      processMessage(bot.messages[i]);
    }
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    yield();
  }
}

void processMessage(telegramMessage msg) {
  String chat_id = String(msg.chat_id);
  String text = msg.text;
  
  // Быстрая проверка доступа
  if(!isUserAllowed(chat_id) && !text.startsWith("/add_user")) {
    bot.sendMessage(chat_id, "⛔ Доступ запрещен. Используйте /add_user [пароль]", "");
    return;
  }

  // Оптимизированная обработка команд
  if (text.startsWith("/add_user ")) {
    handleAddUser(chat_id, text.substring(10));
  } 
  else if (text == "/status") {
    sendCurrentStatus(chat_id);
  }
  else if (text == "/report_on" || text == "/report_off") {
    handleReportToggle(chat_id, text);
  }
  else if (text.startsWith("/set_time ")) {
    handleSetTime(chat_id, text.substring(10));
  }
  else if (text.startsWith("/set_temp_threshold ")) {
    handleSetThreshold(chat_id, text.substring(19));
  }
  else if (text == "/list_users") {
    listUsers(chat_id);
  }
  else if (text == "/help") {
    sendHelp(chat_id);
  }
}

void checkSystem(uint8_t* lastDay) {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo, 100)) { // Таймаут 100 мс
    Serial.println("Ошибка синхронизации времени");
    return;
  }

  if (reportEnabled && timeinfo.tm_hour == reportHour && 
      timeinfo.tm_mday != *lastDay) {
    *lastDay = timeinfo.tm_mday;
    sendDailyReport();
  }

  checkTemperatureThreshold();
}

void handleAddUser(String chat_id, String password) {
  if(password == MASTER_PASSWORD) {
    if(addNewUser(chat_id)) {
      bot.sendMessage(chat_id, "✅ Вы успешно добавлены как новый пользователь!", "");
    } else {
      bot.sendMessage(chat_id, "❌ Не удалось добавить пользователя. Лимит: " + String(MAX_USERS), "");
    }
  } else {
    bot.sendMessage(chat_id, "⛔ Неверный пароль!", "");
  }
}

void sendCurrentStatus(String chat_id) {
  float t = getCurrentTemperature();
  String message = "📊 Текущие показания:\n";
  message += "🌡 Температура: " + String(t) + "°C\n";
  message += "⏰ Московское время: " + getRussianTimeString();
  message += "\n👥 Пользователей: " + String(userCount);
  bot.sendMessage(chat_id, message, "");
}

void handleReportToggle(String chat_id, String command) {
  reportEnabled = (command == "/report_on");
  preferences.putBool("reportEnabled", reportEnabled);
  bot.sendMessage(chat_id, "Ежедневный отчет " + String(reportEnabled ? "включен" : "выключен"), "");
}

void handleSetTime(String chat_id, String timeStr) {
  int newHour = timeStr.toInt();
  if (newHour >= 0 && newHour < 24) {
    reportHour = newHour;
    preferences.putInt("reportHour", newHour);
    bot.sendMessage(chat_id, "⏰ Время отчета установлено на " + String(newHour) + ":00", "");
  } else {
    bot.sendMessage(chat_id, "❌ Некорректное время. Используйте 0-23", "");
  }
}

void handleSetThreshold(String chat_id, String thresholdStr) {
  float newThreshold = thresholdStr.toFloat();
  tempThreshold = newThreshold;
  preferences.putFloat("tempThreshold", newThreshold);
  bot.sendMessage(chat_id, "🌡 Порог температуры установлен на " + String(newThreshold) + "°C", "");
}

void listUsers(String chat_id) {
  if(userCount == 0) {
    bot.sendMessage(chat_id, "Список пользователей пуст", "");
    return;
  }
  
  String userList = "👥 Авторизованные пользователи (" + String(userCount) + "):\n";
  for(int i = 0; i < userCount; i++) {
    userList += allowedChatIds[i] + "\n";
  }
  bot.sendMessage(chat_id, userList, "");
}

void sendHelp(String chat_id) {
  String help = "🤖 Доступные команды:\n\n";
  help += "/add_user [пароль] - регистрация\n";
  help += "/status - текущие показания\n";
  help += "/report_on - включить отчет\n";
  help += "/report_off - выключить отчет\n";
  help += "/set_time [час] - установить время отчета (0-23)\n";
  help += "/set_temp_threshold [значение] - установить порог температуры\n";
  help += "/list_users - список пользователей\n";
  help += "/help - это сообщение";
  bot.sendMessage(chat_id, help, "");
}

bool isUserAllowed(String chat_id) {
  for(int i = 0; i < userCount; i++) {
    if(chat_id == allowedChatIds[i]) {
      return true;
    }
  }
  return false;
}

bool addNewUser(String chat_id) {
  if(userCount >= MAX_USERS) return false;
  if(isUserAllowed(chat_id)) return false;
  
  allowedChatIds[userCount] = chat_id;
  userCount++;
  
  preferences.putInt("userCount", userCount);
  preferences.putString(("user_" + String(userCount-1)).c_str(), chat_id);
  
  return true;
}

float getCurrentTemperature() {
  float temp = sensors.getTempC(tempSensor);
  return temp == DEVICE_DISCONNECTED_C ? NAN : temp;
}

void sendDailyReport() {
  float t = getCurrentTemperature();
  if (isnan(t)) return;
  
  String message = "📊 Ежедневный отчет:\n";
  message += "🌡 Температура: " + String(t) + "°C\n";
  message += "⏰ Время: " + getRussianTimeString();
  
  for(int i = 0; i < userCount; i++) {
    bot.sendMessage(allowedChatIds[i], message, "");
  }
}

void checkTemperatureThreshold() {
  static bool tempAlertSent = false;
  float t = getCurrentTemperature();
  
  if (isnan(t)) return;
  
  if (t > tempThreshold && !tempAlertSent) {
    for(int i = 0; i < userCount; i++) {
      bot.sendMessage(allowedChatIds[i], "⚠️ Внимание! Превышена температура: " + String(t) + "°C", "");
    }
    tempAlertSent = true;
  } else if (t <= tempThreshold) {
    tempAlertSent = false;
  }
}

String getRussianTimeString() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo, 100)) {
    return "Время не синхронизировано";
  }
  
  char timeString[20];
  strftime(timeString, sizeof(timeString), "%d.%m.%Y %H:%M:%S", &timeinfo);
  return String(timeString);
}

void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Подключение к WiFi");
  for(int i = 0; i < 20; i++) {
    if(WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi подключен");
      Serial.print("IP адрес: ");
      Serial.println(WiFi.localIP());
      return;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nОшибка подключения WiFi");
}