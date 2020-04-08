/*
 * hardcore_rtc_init.c
 *
 *  Created on: 8 апр. 2020 г.
 *      Author: snork
 */

#include "rtc.h"

#include <assert.h>
#include <errno.h>

#include "sins_config.h"
#include "time_util.h"

RTC_HandleTypeDef hrtc;


//! Ошибка в терминах ХАЛа в errno
inline static int _hal_status_to_errno(HAL_StatusTypeDef h_status)
{
	int rc;

	switch (h_status)
	{
	case HAL_OK:
		rc = 0;
		break;

	case HAL_BUSY:
		rc = -EBUSY;
		break;

	case HAL_TIMEOUT:
		rc = -ETIMEDOUT;
		break;

	default:
	case HAL_ERROR:
		rc = -EFAULT;
		break;
	}

	return rc;
}


//! Настройка клоков RTC
/*! Это нужно делать, только если вдруг обнаружится что наш бэкап домен нифига не настроен */
static void _init_rtc_clocks()
{
	RCC_OscInitTypeDef RCC_OscInitStruct = {0};

	// Влючаем LSE
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE; // Из всех осциляторов трогаем только LSE
	RCC_OscInitStruct.LSEState = RCC_LSE_ON; // Включаем lSE
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE; // PLL не трогаем
	assert(HAL_RCC_OscConfig(&RCC_OscInitStruct) == HAL_OK);

	RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

	// Указываем LSE как входной такт для RTC
	PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RTC;
	PeriphClkInitStruct.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
	/*
	A caution to be taken when HAL_RCCEx_PeriphCLKConfig() is used to select RTC clock selection, in this case
	the Reset of Backup domain will be applied in order to modify the RTC Clock source as consequence all backup
	domain (RTC and RCC_BDCR register expect BKPSRAM) will be reset
	*/
	assert(HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) == HAL_OK);

	// А еще эта штука забывает выключить доступ на запись к бекап домену. Сделаем это за нее
	HAL_PWR_DisableBkUpAccess();
}


//! Настройка RTC халовского хендла
static void _init_rtc_handle()
{
	hrtc.Instance = RTC;
	hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
	hrtc.Init.AsynchPrediv = 127; // предполагается что на LSE сидит часовой кварц на 32767
	hrtc.Init.SynchPrediv = 255;  // Эти делители дадут нам 1 Герц для секунд
	hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
	hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
	hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
}


void time_svc_rtc_hardcore_init()
{
	// Настриваем LSE и запитываем от него RTC
	_init_rtc_clocks();

	// Настриваем наш хеднл
	_init_rtc_handle();

	// Канонично настраиваем RTC
	HAL_RTC_Init(&hrtc);

	// Выставляем 1 января 2000ого года
	RTC_TimeTypeDef time;

	time.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
	time.Hours = RTC_ByteToBcd2(0);
	time.Minutes = RTC_ByteToBcd2(0);
	time.SecondFraction = 0; // This field will be used only by HAL_RTC_GetTime function
	time.Seconds = RTC_ByteToBcd2(0);
	time.StoreOperation = RTC_STOREOPERATION_RESET;
	time.SubSeconds = RTC_ByteToBcd2(0);
	time.TimeFormat = RTC_HOURFORMAT12_AM; // для 24ти часового формата, который мы подразумеваем нужно так

	RTC_DateTypeDef date;
	date.Date = 1;
	date.Month = RTC_MONTH_JANUARY;
#if ITS_SINS_TIME_SVC_RTC_BASE_YEAR != 2000
#	error "ITS_SINS_TIME_SVC_RTC_BASE_YEAR is not equal to 2000. Reconsider code here"
#endif
	date.WeekDay = RTC_WEEKDAY_SATURDAY; // это была суббота
	date.Year = RTC_ByteToBcd2(0x00);    // Будем считать это 2000ным годом

	HAL_PWR_EnableBkUpAccess();
	HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BCD);
	HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BCD);
	HAL_PWR_DisableBkUpAccess();

	// Ну, типо оно тикает
}


void time_svc_rtc_simple_init()
{
	// к сожалению в ХАЛ-Е не предусмотрены интерфейсы для работы
	// с RTC, который уже настроен кем-то до нас.
	// Поэтому сделаем это все для него и сделаем вид что ХАЛ нормально настроился

	// Настриваем наш хеднл
	_init_rtc_handle();

	// Разрешаем доступ к RTC по соответсвующей APB или какой-нибудь там шине
	// Короче прокидываем себе мостик к RTC в бэкап домен
	// (кажется)
	HAL_RTC_MspInit(&hrtc);

	// Делаем вид что все настроилось как задумано
	hrtc.Lock = HAL_UNLOCKED;
	hrtc.State = HAL_RTC_STATE_READY;

	// Готово
}


void time_svc_rtc_init(void)
{
	if (RCC->BDCR & RCC_BDCR_RTCEN)
	{
		// Значит оно уже работает. Настраиваемся по-простому
		time_svc_rtc_simple_init();
	}
	else
	{
		// Значит оно не работает. Запускаем по жести
		time_svc_rtc_hardcore_init();
	}
}


int time_svc_rtc_load(struct tm * tm)
{
	int rc;
	HAL_StatusTypeDef hrc_date, hrc_time;

	// Сперва все вчитаем а потом будет разбираться с ошибками
	RTC_DateTypeDef rtc_date;
	RTC_TimeTypeDef rtc_time;
	hrc_date = HAL_RTC_GetDate(&hrtc, &rtc_date, RTC_FORMAT_BCD);
	hrc_time = HAL_RTC_GetTime(&hrtc, &rtc_time, RTC_FORMAT_BCD);

	rc = _hal_status_to_errno(hrc_date);
	if (0 != rc)
		return rc;

	rc = _hal_status_to_errno(hrc_time);
	if (0 != rc)
		return rc;

	// Пересчитываем в struct tm
	rc = time_svc_rtc_to_struct_tm(&rtc_date, &rtc_time, tm);
	if (0 != rc)
		return rc;

	return 0;
}


int time_svc_rtc_store(const struct tm * tm)
{
	int rc;
	HAL_StatusTypeDef hrc_date, hrc_time;

	// Кастуем в халовские структуры
	RTC_DateTypeDef rtc_date;
	RTC_TimeTypeDef rtc_time;

	rc = time_svc_struct_tm_to_rtc(tm, &rtc_date, &rtc_time);
	if (0 != rc)
		return rc;

	// зашиваем!
	hrc_date = HAL_RTC_SetDate(&hrtc, &rtc_date, RTC_FORMAT_BCD);
	hrc_time = HAL_RTC_SetTime(&hrtc, &rtc_time, RTC_FORMAT_BCD);

	// Проверяем как зашилось
	rc = _hal_status_to_errno(hrc_date);
	if (0 != rc)
		return rc;

	rc = _hal_status_to_errno(hrc_time);
	if (0 != rc)
		return rc;

	return 0;
}


int time_svc_rtc_alarm_setup(const struct tm * tm, uint32_t alarm)
{
	int rc;
	HAL_StatusTypeDef hrc;

	// Кастуем в халовские структуры
	RTC_AlarmTypeDef rtc_alarm;
	RTC_DateTypeDef rtc_date;

	rc = time_svc_struct_tm_to_rtc(tm, &rtc_date, &rtc_alarm.AlarmTime);
	if (0 != rc)
		return rc;

	rtc_alarm.AlarmMask = RTC_ALARMMASK_NONE; 	// Алармим по всем параметрам даты
	rtc_alarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL; // Забиваем на сабсекунды
	rtc_alarm.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE; // Работаем по дате а не еженедельно
	rtc_alarm.AlarmDateWeekDay = rtc_date.Date; // Уточняем собственно дату
	rtc_alarm.Alarm = alarm;

	// Выставляемs
	hrc = HAL_RTC_SetAlarm(&hrtc, &rtc_alarm, RTC_FORMAT_BCD);
	rc = _hal_status_to_errno(hrc);
	if (0 != rc)
		return rc;

	return 0;
}
