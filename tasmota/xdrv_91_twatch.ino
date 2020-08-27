/*
  xdrv_91_twatch.ino

  Copyright (C) 2020  Christian Baars & Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef ESP32
#define USE_TTGO
#define LILYGO_WATCH_2020_V1
#define LILYGO_WATCH_LVGL                   //To use LVGL, you need to enable the macro LVGL

#ifdef USE_TTGO

#define XDRV_91           91
#include <LilyGoWatch.h>
#include <lvgl/lvgl.h>
#include "esp32-hal-cpu.h"

TTGOClass *ttgo;
LV_IMG_DECLARE(tm_logo_name);
LV_IMG_DECLARE(tm_logo_T);
LV_IMG_DECLARE(tm_logo_O);
LV_IMG_DECLARE(tm_wifi_on);
LV_IMG_DECLARE(tm_wifi_off);
LV_IMG_DECLARE(tm_charge);
LV_IMG_DECLARE(tm_battery);
LV_IMG_DECLARE(tm_colorpicker);
LV_IMG_DECLARE(tm_calculator);
LV_IMG_DECLARE(tm_console);
LV_IMG_DECLARE(tm_tool);

struct {
  lv_style_t style;
  lv_obj_t *date;
  lv_obj_t *time;
  lv_obj_t *version;
  lv_obj_t *pictPower;
  lv_obj_t *battLevel;
  lv_obj_t *pictWifi;
  lv_obj_t *logo_O;
  lv_obj_t *mboxWifi;
  lv_obj_t *toolContainer;
  lv_obj_t *batteryFilling;
  lv_obj_t *upperInfoLabel;
  lv_obj_t *calculatorNum1;
  lv_obj_t *calculatorNum2;
  lv_obj_t *calculatorResult;
  struct{
    uint32_t isCharging:1;
    uint32_t batteryConnected:1;
    uint32_t isVBUSPlug:1;
    uint32_t lightSleep:1;
    uint32_t timeWasSynced:1;
    uint32_t singleButtonPress:1;
  } state;
  uint32_t screenTimer = 200;
  uint8_t statusMessage = 0;
  bool irq;
} TTGO;

void TTGOBtnPressed(){
  if(TTGO.state.lightSleep) {
    TTGOwake();
  }
  else{
    TTGO.state.singleButtonPress=1;
  }
}

void TTGOwake(){
  ttgo->startLvglTick();
  ttgo->rtc->syncToSystem();
  lv_disp_trig_activity(NULL);
  ttgo->displayWakeup();
  ttgo->openBL();
  TTGO.state.lightSleep = 0;
  if(TTGO.state.isVBUSPlug) {
    setCpuFrequencyMhz(80);
  }
  else setCpuFrequencyMhz(40);
}

void TTGOlightSleep(){
  ttgo->closeBL();
  ttgo->stopLvglTick();
  ttgo->bma->enableStepCountInterrupt(false);
  ttgo->displaySleep();
  TTGO.state.lightSleep = 1;
  setCpuFrequencyMhz(10);
}

void TTGOinitTime(void){
  ttgo->rtc->check();
  RTC_Date _time = ttgo->rtc->getDateTime();

  TIME_T _sysTime;
  _sysTime.year = _time.year - 1970;
  _sysTime.month = _time.month;
  _sysTime.day_of_month = _time.day;
  _sysTime.hour = _time.hour;
  _sysTime.minute = _time.minute;
  _sysTime.second = _time.second;
  
  Rtc.utc_time = MakeTime(_sysTime);
}

void TTGOInitPower(void){
  pinMode(AXP202_INT, INPUT_PULLUP);
  attachInterrupt(AXP202_INT, [] {
      TTGO.irq = true;
  }, FALLING);
  ttgo->power->enableIRQ(AXP202_PEK_SHORTPRESS_IRQ| AXP202_VBUS_REMOVED_IRQ | AXP202_VBUS_CONNECT_IRQ | AXP202_CHARGING_IRQ,
                    true);
  ttgo->power->clearIRQ();
  ttgo->power->adc1Enable(AXP202_VBUS_VOL_ADC1 | AXP202_VBUS_CUR_ADC1 | AXP202_BATT_CUR_ADC1 | AXP202_BATT_VOL_ADC1, true);
  ttgo->power->setPowerOutPut(AXP202_EXTEN, AXP202_OFF);
  ttgo->power->setPowerOutPut(AXP202_DCDC2, AXP202_OFF);
  ttgo->power->setPowerOutPut(AXP202_LDO3, AXP202_OFF);
  ttgo->power->setPowerOutPut(AXP202_LDO4, AXP202_OFF);
  TTGO.state.isVBUSPlug = ttgo->power->isVBUSPlug();
}

void TTGOInitGyro(void){
  ttgo->bma->begin();
  Acfg _cfg;
  _cfg.odr = BMA4_OUTPUT_DATA_RATE_100HZ;
  _cfg.range = BMA4_ACCEL_RANGE_2G;
  _cfg.bandwidth = BMA4_ACCEL_NORMAL_AVG4;
  _cfg.perf_mode = BMA4_CONTINUOUS_MODE;
  
  ttgo->bma->accelConfig(_cfg);
  // Warning : Need to use feature, you must first enable the accelerometer
  ttgo->bma->enableAccel();
  ttgo->bma->attachInterrupt();
  pinMode(BMA423_INT1, INPUT);
  attachInterrupt(BMA423_INT1, [] {
      TTGO.irq = 1;
  }, RISING); //It must be a rising edge

  ttgo->bma->enableFeature(BMA423_STEP_CNTR, true);
  ttgo->bma->enableFeature(BMA423_TILT, true);
  // Enable BMA423 isDoubleClick feature
  ttgo->bma->enableFeature(BMA423_WAKEUP, true);

  // Reset steps
  ttgo->bma->resetStepCounter();

  // Turn on feature interrupt
  ttgo->bma->enableStepCountInterrupt();
  ttgo->bma->enableTiltInterrupt();
  // It corresponds to isDoubleClick interrupt
  ttgo->bma->enableWakeupInterrupt();
}

void TTGOwifiTouchCB(lv_obj_t * obj, lv_event_t event);
void TTGOpowerCB(lv_obj_t * obj, lv_event_t event);
static void TTGOwifiBoxevent_handler(lv_obj_t * obj, lv_event_t e);
static void TTGOpowerBoxevent_handler(lv_obj_t * obj, lv_event_t e);
void TTGOcalculatorCB(lv_obj_t * obj, lv_event_t event);
void TTGOstartColorPicker(lv_obj_t * obj, lv_event_t event);
void TTGOstartCalculator(lv_obj_t * obj, lv_event_t event);



void TTGOwifiTouchCB(lv_obj_t * obj, lv_event_t event){
  if(event==LV_EVENT_SHORT_CLICKED){
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("Wifi Status: %d"),WifiState());

    lv_obj_t *tabview;
    tabview = lv_tabview_create(lv_scr_act(), NULL);
    // lv_obj_set_width(tabview, 220);
    lv_obj_add_style(tabview, LV_TABVIEW_PART_TAB_BG, &TTGO.style);
    lv_obj_add_style(tabview, LV_TABVIEW_PART_BG, &TTGO.style);
    lv_obj_t *tab1 = lv_tabview_add_tab(tabview, "Wifi");
    lv_obj_t *tab2 = lv_tabview_add_tab(tabview, "MQTT");

    lv_obj_t * label = lv_label_create(tab1, NULL);
    char _buf[300];

    sprintf(_buf, "Wifi:\n"
                  "IP: %s\n"
                  "MAC: %s\n"
                  "Gateway: %s\n",
                  WiFi.localIP().toString().c_str(),WiFi.macAddress().c_str(),
                  IPAddress(Settings.ip_address[1]).toString().c_str()
                  );

    lv_label_set_text(label,_buf);

    label = lv_label_create(tab2, NULL);
    sprintf(_buf, "MQTT:\n"
                  "Host: %s\n"
                  "Port: %u\n"
                  "User: %s\n"
                  "Client: %s\n"
                  "Topic: %s\n",
                  SettingsText(SET_MQTT_HOST),Settings.mqtt_port,SettingsText(SET_MQTT_USER),
                  mqtt_client,SettingsText(SET_MQTT_TOPIC)
                  );
    lv_label_set_text(label, _buf);

    lv_obj_t * btnLabel;
    lv_obj_t * btn1 = lv_btn_create(tabview, NULL);
    lv_obj_set_event_cb(btn1, TTGOpowerBoxevent_handler);
    lv_obj_align(btn1, tabview, LV_ALIGN_IN_BOTTOM_RIGHT, 0, 0);

    btnLabel = lv_label_create(btn1, NULL);
    lv_label_set_text(btnLabel, "Close");
    TTGO.screenTimer += 200;
  }
}

static void TTGOwifiBoxevent_handler(lv_obj_t * obj, lv_event_t e)
{
  if(e == LV_EVENT_CLICKED) {
    lv_obj_del(lv_obj_get_parent(obj));
  }
}

static void TTGOpowerBoxevent_handler(lv_obj_t * obj, lv_event_t e)
{
  if(e == LV_EVENT_CLICKED) {
    lv_obj_del(lv_obj_get_parent(obj));
  }
}

void TTGOcalculatorCB(lv_obj_t * obj, lv_event_t event){
  static double _num1 = 0.0;
  static double _num2 = 0.0;
  static char _op = 0;
  static bool _havePoint = false;
  static bool _justFinished = false;
  static bool _firstRunOver = false;
  static uint32_t _digitsLeft=0;
  static uint32_t _digitsRight=0;
  float _result;

  if(event == LV_EVENT_VALUE_CHANGED) {
    const char * _inputtxt = lv_btnmatrix_get_active_btn_text(obj);
    char _txt;
    memcpy(&_txt,_inputtxt,1);
    switch (_txt){
    case 'C':
      lv_label_set_text(TTGO.calculatorNum2,"0");
      _havePoint = false;
      _digitsLeft = 0;
      _num2 = 0;
      _firstRunOver = false;
      break;
    case '+': case '-': case '/' :case 'x': case '=':
      _justFinished=true;
      if(_txt!='=') {
        _op = _txt;
        if(!_firstRunOver){
          _num1 = _num2;
          _firstRunOver = true;
        }
        break;
      }
      if(_op=='+'){
        _result = _num1+_num2;
      }
      else if(_op=='-'){
        _result = _num1+-_num2;
      }
      else if(_op=='/'){
        _result = _num1/_num2;
      }
      else if(_op=='x'){
        _result = _num1*_num2;
      }
      _num1 = _result;
      break;
    case '.':
      if(_havePoint){
        break;
        }
      _havePoint=true;
    default:{
        if(_justFinished) {
          _havePoint=false;
          lv_label_set_text(TTGO.calculatorNum2,"0");
          _digitsRight=0;
          _justFinished=false;
          _num2=0;
        }
        const char *_oldtext = lv_label_get_text(TTGO.calculatorNum2);
        size_t _len = strlen(_oldtext);
        char _newtext[_len+1];
        memcpy((char*)&_newtext,_oldtext,_len);
        if(_newtext[_len-1]=='0') _len-=1;
        _newtext[_len]=_txt;
        _newtext[_len+1]=0;

        lv_label_set_text(TTGO.upperInfoLabel,_inputtxt);

        lv_label_set_text(TTGO.calculatorNum2,(const char *)&_newtext);
        if(_newtext[_len]=='.') break;
        if(!_havePoint){
          _digitsLeft++;
          _num2=(_num2*10.0)+atoi(_inputtxt);
          // lv_label_set_text_fmt(TTGO.upperInfoLabel," %f",_num2);
        }
        else{
          _digitsRight++;
          double _resultPOW = 1;
          for (int i = 1; i<=_digitsRight; ++i)
              _resultPOW *= 10;
          double _fractions=atof(_inputtxt)/_resultPOW;
          _num2=_num2+_fractions;
          // lv_label_set_text_fmt(TTGO.upperInfoLabel,". %f",_num2);
        }
      }
    }
      if(_txt!='=')lv_label_set_text_fmt(TTGO.calculatorNum1,"%.04f  %c", _num1, _op);
      lv_label_set_text_fmt(TTGO.calculatorResult,"%f",_result);
      lv_obj_align(TTGO.calculatorNum1, NULL, LV_ALIGN_IN_TOP_MID, 0, 20);
      lv_obj_align(TTGO.calculatorNum2, TTGO.calculatorNum1, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
      lv_obj_align(TTGO.calculatorResult, TTGO.calculatorNum2, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
      TTGO.screenTimer += 100;
  }
}



void TTGOpowerCB(lv_obj_t * obj, lv_event_t event){
  if(event==LV_EVENT_SHORT_CLICKED){
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("Power Click"));
    /*Create a Tab view object*/
    lv_obj_t *tabview;
    tabview = lv_tabview_create(lv_scr_act(), NULL);
    lv_obj_add_style(tabview, LV_TABVIEW_PART_TAB_BG, &TTGO.style);
    lv_obj_add_style(tabview, LV_TABVIEW_PART_BG, &TTGO.style);
    lv_obj_t *tab1 = lv_tabview_add_tab(tabview, "Info");
    lv_obj_t *tab2 = lv_tabview_add_tab(tabview, "Setting");
    lv_obj_t *tab3 = lv_tabview_add_tab(tabview, "Chip");

    lv_obj_t * label = lv_label_create(tab1, NULL);
    char _buf[300];

    sprintf(_buf, "VBUS:\n"
                      "Voltage: %.02f mV\n"
                      "Current: %.02f mV\n"
                      "Battery:\n"
                      "Voltage: %.02f mV\n"
                      "DischargeCur: %.02f mA\n"
                      "Level: %u %%\n"
                      "CPU:\n"
                      "Frequency: %u MHz\n\n\n\n",
                      ttgo->power->getVbusVoltage(),ttgo->power->getVbusCurrent(),
                      ttgo->power->getBattVoltage(),
                      ttgo->power->getBattDischargeCurrent(),ttgo->power->getBattPercentage(),
                      getCpuFrequencyMhz()
                      );

    lv_label_set_text(label,_buf);

    label = lv_label_create(tab2, NULL);
    lv_label_set_text(label, "Display off after x seconds\n"
                            "Deep sleep after x minutes\n"
                            "Deep sleep at battery level of \n"
                            "CPU max. Frequency"
    );


    sprintf(_buf, "Chip:\n"
                  "ID: %d\n"
                  "Flash size: %d kB\n"
                  "Prog flash size: %d kB\n"
                  "Sketch size: %d kB\n"
                  "Free sketch size: %d kB\n"
                  "Free memory: %d kB\n"
                  "Max PS memory: %d kB\n"
                  "Free PS memory: %d kB\n\n\n\n",
                  ESP_getChipId(),ESP.getFlashChipRealSize() / 1024,
                  ESP.getFlashChipSize() / 1024,ESP_getSketchSize() / 1024, 
                  ESP.getFreeSketchSpace() / 1024,ESP_getFreeHeap() / 1024,
                  ESP.getPsramSize() / 1024,ESP.getFreePsram() / 1024
                  );
    label = lv_label_create(tab3, NULL);
    lv_label_set_text(label,_buf);

    lv_obj_t * btnLabel;
    lv_obj_t * btn1 = lv_btn_create(tabview, NULL);
    lv_obj_set_event_cb(btn1, TTGOpowerBoxevent_handler);
    lv_obj_align(btn1, tabview, LV_ALIGN_IN_BOTTOM_RIGHT, 0, 0);

    btnLabel = lv_label_create(btn1, NULL);
    lv_label_set_text(btnLabel, "Close");
    TTGO.screenTimer += 200;
  }
}

void TTGOstartColorPicker(lv_obj_t * obj, lv_event_t event){
  if(event==LV_EVENT_SHORT_CLICKED){
    TTGO.toolContainer = lv_obj_create(lv_obj_get_parent(obj), NULL);
    lv_obj_set_size(TTGO.toolContainer, LV_HOR_RES, LV_VER_RES);
    // lv_obj_set_pos(TTGO.toolContainer, 0, LV_VER_RES);
    lv_obj_t * cpicker = lv_cpicker_create(TTGO.toolContainer, NULL);
    lv_obj_set_size(cpicker, 200, 200);
    lv_obj_align(cpicker, NULL, LV_ALIGN_CENTER, 0, 0);
    TTGO.screenTimer += 200;
  }
}

void TTGOstartCalculator(lv_obj_t * obj, lv_event_t event){
  if(event==LV_EVENT_SHORT_CLICKED){
    TTGO.toolContainer = lv_obj_create(lv_obj_get_parent(obj), NULL);
    lv_obj_set_size(TTGO.toolContainer, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_local_text_font(TTGO.toolContainer,LV_OBJ_PART_MAIN,LV_STATE_DEFAULT, LV_THEME_DEFAULT_FONT_SMALL);
    static const char * btnm_map[] = {
                                    "7", "8", "9", "/","\n",
                                    "4", "5", "6", "x", "\n",
                                    "1", "2", "3", "-", "\n",
                                    "0", ".", "C", "+", "\n",
                                    "=","\n",
                                    ""};

    TTGO.calculatorNum1 = lv_label_create(TTGO.toolContainer, NULL);
    lv_label_set_text(TTGO.calculatorNum1, "0");
    lv_obj_align(TTGO.calculatorNum1, NULL, LV_ALIGN_IN_TOP_MID, -20, 20);
    TTGO.calculatorNum2 = lv_label_create(TTGO.toolContainer, TTGO.calculatorNum1);
    lv_obj_align(TTGO.calculatorNum2, TTGO.calculatorNum1, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    TTGO.calculatorResult = lv_label_create(TTGO.toolContainer, TTGO.calculatorNum1);
    lv_obj_align(TTGO.calculatorResult, TTGO.calculatorNum2, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);


    static lv_style_t style_btnm;
    lv_obj_t * btnm1 = lv_btnmatrix_create(TTGO.toolContainer, NULL);
    lv_btnmatrix_set_map(btnm1, btnm_map);
    lv_obj_add_style(btnm1,LV_STATE_DEFAULT,&style_btnm);
    lv_obj_align(btnm1, NULL, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_event_cb(btnm1, TTGOcalculatorCB);
    TTGO.screenTimer += 200;
  }
}



void TTGOtimeUpdateTask(void){
        RTC_Date _time = ttgo->rtc->getDateTime();
        lv_label_set_text_fmt(TTGO.time, "%02u:%02u",_time.hour,_time.minute);
        lv_img_set_angle(TTGO.logo_O, ((_time.second)*60)+1800);
}


void TTGOrestUpdateTask(void){
      TIME_T _tm;
      BreakTime(Rtc.local_time,_tm) ;
      lv_label_set_text_fmt(TTGO.date, "%u. %s %u", _tm.day_of_month,_tm.name_of_month,_tm.year+1970);

      if (TTGO.state.isVBUSPlug){
        lv_img_set_src(TTGO.pictPower, &tm_charge);
      }
      else{
        lv_img_set_src(TTGO.pictPower, &tm_battery);
      }
      int _level = ttgo->power->getBattPercentage();
      lv_label_set_text_fmt(TTGO.battLevel, "%u%%",_level);
      if(WifiState()==-1){
        lv_img_set_src(TTGO.pictWifi, &tm_wifi_off);
      }
      else{
        lv_img_set_src(TTGO.pictWifi, &tm_wifi_on);
      }
    lv_obj_set_size(TTGO.batteryFilling, (uint16_t)_level/100.0*15.0, 9);
    if(_level>20){
      lv_obj_set_style_local_bg_color(TTGO.batteryFilling, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0x7c,0xFC,0x00));
    }
    else if(_level>10){
      lv_obj_set_style_local_bg_color(TTGO.batteryFilling, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0xFF,0xFF,0x33));
    }
    else{
      lv_obj_set_style_local_bg_color(TTGO.batteryFilling, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0xFF,0x00,0x00));
    }

    static uint32_t _counter = 5;
    if(TTGO.statusMessage!=0){
      _counter=5;
      switch(TTGO.statusMessage){
        case 1:
          lv_label_set_text(TTGO.upperInfoLabel,"8.1.6 by Theo Arends");
          break;
        case 2:
          lv_label_set_text(TTGO.upperInfoLabel,"Uptime: ");
          break;
      }
      lv_obj_align(TTGO.upperInfoLabel, NULL, LV_ALIGN_CENTER, 0, 0);
    }

    if(_counter==0){
      lv_label_set_text(TTGO.upperInfoLabel,"");
      _counter=5;
    }
    _counter--;
}



void TTGOInitWatchFace(void){
    ttgo->lvgl_begin();
    // Turn on the backlight
    ttgo->openBL();
    //Lower the brightness
    ttgo->bl->adjust(20);

    // style
    lv_style_init(&TTGO.style);
    lv_style_set_text_color(&TTGO.style, LV_STATE_DEFAULT, LV_COLOR_SILVER);
    lv_style_set_bg_color(&TTGO.style, LV_STATE_DEFAULT, LV_COLOR_BLACK);
    lv_style_set_text_font(&TTGO.style, LV_STATE_DEFAULT, LV_THEME_DEFAULT_FONT_SMALL);

    //background
    lv_obj_t * mainScreen = lv_scr_act();
    lv_obj_add_style(mainScreen,LV_OBJ_PART_MAIN, &TTGO.style);

    static lv_point_t valid_pos[] = {{0,0}, {0, 1},{1,1},{2,1},{3,1}};
    lv_obj_t *tileview;
    tileview = lv_tileview_create(mainScreen, NULL);
    lv_page_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_style(tileview,LV_OBJ_PART_MAIN, &TTGO.style);
    lv_tileview_set_valid_positions(tileview, valid_pos, 5);
    lv_tileview_set_edge_flash(tileview, true);

    lv_obj_t * frontPageTile = lv_obj_create(tileview, NULL);
    lv_obj_set_size(frontPageTile, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(frontPageTile, 0, 0);
    lv_tileview_add_element(tileview, frontPageTile);

    lv_obj_t *logo_name = lv_img_create(frontPageTile, NULL);
    lv_img_set_src(logo_name,&tm_logo_name);
    lv_obj_align(logo_name, frontPageTile, LV_ALIGN_IN_BOTTOM_RIGHT, -18, 0);
    // lv_obj_set_event_cb(logo_name,TTGOnameLabelCB);
    

    TTGO.logo_O = lv_img_create(frontPageTile, NULL);
    lv_img_set_src(TTGO.logo_O, &tm_logo_O);
    lv_obj_align(TTGO.logo_O, logo_name, LV_ALIGN_OUT_TOP_MID, 0, -50);
    
    lv_obj_t *logo_T = lv_img_create(frontPageTile, NULL);
    lv_img_set_src(logo_T, &tm_logo_T);
    lv_obj_align(logo_T, TTGO.logo_O, LV_ALIGN_OUT_TOP_MID, 0, 40);

    // date and time
    TTGO.date = lv_label_create(frontPageTile, nullptr);
    lv_obj_add_style(TTGO.date, LV_OBJ_PART_MAIN, &TTGO.style);
    lv_obj_align(TTGO.date, TTGO.logo_O, LV_ALIGN_IN_BOTTOM_LEFT,-114, -40);

    TTGO.time = lv_label_create(frontPageTile, nullptr);
    lv_label_set_text(TTGO.time, " ");
    lv_obj_align(TTGO.time, TTGO.date, LV_ALIGN_IN_BOTTOM_LEFT, 0, 15);
    lv_obj_set_style_local_text_font(TTGO.time,LV_OBJ_PART_MAIN,LV_STATE_DEFAULT, LV_THEME_DEFAULT_FONT_TITLE);
    // lv_obj_set_event_cb(TTGO.time,TTGOtimeLabelCB);

    // Toolpage 1
    lv_obj_t * toolTile1 = lv_obj_create(tileview, NULL);
    lv_obj_set_size(toolTile1, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(toolTile1, 0, LV_VER_RES);
    lv_tileview_add_element(tileview, toolTile1);

    lv_obj_t * colorPicker = lv_imgbtn_create(toolTile1, NULL);
    lv_imgbtn_set_src(colorPicker,LV_BTN_STATE_RELEASED, &tm_colorpicker);
    lv_imgbtn_set_src(colorPicker,LV_BTN_STATE_PRESSED, &tm_colorpicker);
    lv_obj_align(colorPicker, NULL, LV_ALIGN_IN_TOP_LEFT, 30, 50);
    lv_obj_set_event_cb(colorPicker,TTGOstartColorPicker);


    lv_obj_t * calculator = lv_imgbtn_create(toolTile1, NULL);
    lv_imgbtn_set_src(calculator,LV_BTN_STATE_RELEASED, &tm_calculator);
    lv_imgbtn_set_src(calculator,LV_BTN_STATE_PRESSED, &tm_calculator);
    lv_obj_align(calculator, NULL, LV_ALIGN_IN_TOP_LEFT, 150, 50);
    lv_obj_set_event_cb(calculator,TTGOstartCalculator);

    // Toolpage 2
    lv_obj_t * toolTile2 = lv_obj_create(tileview, NULL);
    lv_obj_set_size(toolTile2, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(toolTile2, LV_HOR_RES, LV_VER_RES);
    lv_tileview_add_element(tileview, toolTile2);

    lv_obj_t * console = lv_imgbtn_create(toolTile2, NULL);
    lv_imgbtn_set_src(console,LV_BTN_STATE_RELEASED, &tm_console);
    lv_imgbtn_set_src(console,LV_BTN_STATE_PRESSED, &tm_console);
    lv_obj_align(console, NULL, LV_ALIGN_IN_TOP_LEFT, 20, 50);

    lv_obj_t * tool = lv_imgbtn_create(toolTile2, NULL);
    lv_imgbtn_set_src(tool,LV_BTN_STATE_RELEASED, &tm_tool);
    lv_imgbtn_set_src(tool,LV_BTN_STATE_PRESSED, &tm_tool);
    lv_obj_align(tool, NULL, LV_ALIGN_IN_TOP_LEFT, 140, 50);


    // //Calendar
    // lv_obj_set_style_local_text_font(calendar, LV_CALENDAR_PART_DATE, LV_STATE_DEFAULT, lv_theme_get_font_small());
    // lv_obj_set_style_local_text_font(calendar, LV_CALENDAR_PART_HEADER, LV_STATE_DEFAULT, lv_theme_get_font_small());
    // lv_obj_set_style_local_text_font(calendar, LV_CALENDAR_PART_DAY_NAMES, LV_STATE_DEFAULT, lv_theme_get_font_small());
    // /*Set some date*/
    // lv_calendar_date_t today;
    // today.year = 2018;
    // today.month = 10;
    // today.day = 23;
    // lv_calendar_set_today_date(calendar, &today);
    // lv_calendar_set_showed_date(calendar, &today);


    // top items
    lv_obj_t * topLeft = lv_obj_create(mainScreen, mainScreen);
    lv_obj_set_size(topLeft, 60, 20);
    lv_obj_t * topRight = lv_obj_create(mainScreen, topLeft);
    lv_obj_set_pos(topRight, 180, 0);
    lv_obj_t * topMid = lv_obj_create(mainScreen, topLeft);
    lv_obj_set_pos(topMid, 60, 0);
    lv_obj_set_size(topMid, 120, 20);
    TTGO.upperInfoLabel = lv_label_create(topMid, NULL);
    lv_label_set_text(TTGO.upperInfoLabel, LV_SYMBOL_DOWN "...swipe..." LV_SYMBOL_DOWN);
    lv_obj_align(TTGO.upperInfoLabel, NULL, LV_ALIGN_CENTER, 0, 0);

    // callbacks
    lv_obj_set_event_cb(topLeft,TTGOpowerCB);
    lv_obj_set_event_cb(topRight,TTGOwifiTouchCB);

    TTGO.pictPower = lv_img_create(topLeft, NULL);
    lv_obj_align(TTGO.pictPower, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);
    lv_img_set_src(TTGO.pictPower, &tm_charge);

    TTGO.batteryFilling = lv_obj_create(topLeft, NULL);
    lv_obj_set_size(TTGO.batteryFilling, 14, 9); // 15 = 100%
    lv_obj_set_pos(TTGO.batteryFilling, 2, 5);
    lv_obj_set_style_local_bg_color(TTGO.batteryFilling, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0x7c,0xFC,0x00));

    TTGO.pictWifi = lv_img_create(topRight, NULL);
    lv_obj_align(TTGO.pictWifi, NULL, LV_ALIGN_IN_TOP_LEFT, 40, 0);
    lv_img_set_src(TTGO.pictWifi, &tm_wifi_on);

    TTGO.battLevel = lv_label_create(topLeft, TTGO.date);
    
    lv_obj_align(TTGO.battLevel,TTGO.pictPower, LV_ALIGN_OUT_RIGHT_MID, 5, 0);

    //start top mid
    lv_tileview_set_tile_act(tileview,1,0,LV_ANIM_OFF);

    // task section
    lv_task_create((lv_task_cb_t)TTGOtimeUpdateTask, 500, LV_TASK_PRIO_HIGH, nullptr);
    lv_task_create((lv_task_cb_t)TTGOrestUpdateTask , 1000, LV_TASK_PRIO_LOW, nullptr);
}

void TTGOInit(void){
      //Get watch instance and init
    ttgo = TTGOClass::getWatch();
    ttgo->begin();
    TTGO.screenTimer = 600;

    TTGOinitTime();
    TTGOInitPower();
    TTGOInitWatchFace();
    TTGOInitGyro();
}

void TTGOirqscheduler(){
  ttgo->power->readIRQ();
  if (ttgo->power->isVbusPlugInIRQ()) {
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("Power Plug In"));
    TTGO.state.isVBUSPlug = 1;
    setCpuFrequencyMhz(160);
  }
  if (ttgo->power->isVbusRemoveIRQ()) {
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("Power Remove"));
    TTGO.state.isVBUSPlug = 0;
    setCpuFrequencyMhz(80);
  }
  if (ttgo->power->isPEKShortPressIRQ()) {
    TTGOBtnPressed();
  }
  ttgo->power->clearIRQ();

  while (!ttgo->bma->readInterrupt());
  if (ttgo->bma->isStepCounter()) {
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("Step"));
  }
  if (ttgo->bma->isTilt()) {
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("Tilt"));
    TTGO.screenTimer += 200;
  }
  if (ttgo->bma->isDoubleClick()) {
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("Double Click"));
    TTGO.screenTimer += 200;
    if(TTGO.state.lightSleep) TTGOwake();
  }
}

void TTGOLoop(void){
  static uint32_t _batteryTimer = 30; // 10 * 30 = 300  seconds
  if(TTGO.irq){
    TTGOirqscheduler();
    TTGO.irq=false;
  }
  if(TTGO.state.singleButtonPress){
    if(TTGO.toolContainer) {
      lv_obj_del(TTGO.toolContainer);
      TTGO.toolContainer = nullptr;
      }
    TTGO.state.singleButtonPress  = 0;
  }

  lv_task_handler();
  if(_batteryTimer==0){
    // TODO: if(too low) deep sleep
  }

  if(TTGO.screenTimer==0){
    TTGO.screenTimer = 400;
    _batteryTimer--;
    if(!TTGO.state.lightSleep) TTGOlightSleep();
  }
  TTGO.screenTimer--;

  if(!TTGO.state.timeWasSynced){
    if(!bNetIsTimeSync){
      AddLog_P2(LOG_LEVEL_DEBUG, PSTR("set RTC from NTP"));
      TTGO.state.timeWasSynced=1;
      TIME_T _tm;
      BreakTime(Rtc.local_time,_tm) ;
      ttgo->rtc->setDateTime(_tm.year,_tm.month, _tm.day_of_month, _tm.hour, _tm.minute, _tm.second);
    }
  }
}

void TTGOShow(bool json){
  if (json) {
    if (TTGO.state.isVBUSPlug) ResponseAppend_P(PSTR(",\"Bus\":{\"Voltage\":%.02f mV,\"Current\":%.02f mA}"),ttgo->power->getVbusVoltage(),ttgo->power->getVbusCurrent());
    if (ttgo->power->isBatteryConnect()) ResponseAppend_P(PSTR(",\"Bat\":{\"Voltage\":%.02f mV}"),ttgo->power->getBattVoltage());
    if (ttgo->power->isChargeing()){
      ResponseAppend_P(PSTR(",\"Bat\":{\"Current\":%.02f mA}"),ttgo->power->getVbusCurrent());
    }
    else{
      ResponseAppend_P(PSTR(",\"Bat\":{\"DischargeCurrent\":%.02f mA,\"Level\":%u}"),ttgo->power->getBattDischargeCurrent(),ttgo->power->getBattPercentage());
    }
    Accel _acc;
    if(ttgo->bma->getAccel(_acc))ResponseAppend_P(PSTR(",\"Acc\":{\"X\":%d,\"Y\":%d,\"Z\":%d"),_acc.x,_acc.y,_acc.z);
    ResponseAppend_P(PSTR(",\"ESP\":{\"CPUfrequency\":%u MHz}"),getCpuFrequencyMhz());
#ifdef USE_WEBSERVER
  } else {

#endif  // USE_WEBSERVER
  }
}



/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv91(uint8_t function) {
  bool result = false;

  switch (function) {
    case FUNC_INIT:
      TTGOInit();
      break;
    case FUNC_EVERY_50_MSECOND:
      TTGOLoop();
      break;
    case FUNC_JSON_APPEND:
      TTGOShow(true);
      break;
  }
  return result;
}

#endif  // USE_TTGO
#endif  // ESP32
