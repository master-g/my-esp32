#include "weather_mapper.h"

const char *weather_mapper_text_for_code(uint16_t weather_code, bool is_day)
{
    switch (weather_code) {
    case 0:
        return is_day ? "Clear" : "Clear night";
    case 1:
        return is_day ? "Mostly clear" : "Mostly clear";
    case 2:
        return "Partly cloudy";
    case 3:
        return "Overcast";
    case 45:
    case 48:
        return "Fog";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
        return "Drizzle";
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
    case 80:
    case 81:
    case 82:
        return "Rain";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return "Snow";
    case 95:
    case 96:
    case 99:
        return "Thunder";
    default:
        return "Unknown";
    }
}

weather_icon_t weather_mapper_icon_for_code(uint16_t weather_code, bool is_day)
{
    switch (weather_code) {
    case 0:
        return is_day ? WEATHER_ICON_CLEAR_DAY : WEATHER_ICON_CLEAR_NIGHT;
    case 1:
    case 2:
        return is_day ? WEATHER_ICON_PARTLY_CLOUDY_DAY : WEATHER_ICON_PARTLY_CLOUDY_NIGHT;
    case 3:
        return WEATHER_ICON_CLOUDY;
    case 45:
    case 48:
        return WEATHER_ICON_FOG;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
        return WEATHER_ICON_DRIZZLE;
    case 61:
    case 63:
    case 80:
    case 81:
        return WEATHER_ICON_RAIN;
    case 65:
    case 66:
    case 67:
    case 82:
        return WEATHER_ICON_HEAVY_RAIN;
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return WEATHER_ICON_SNOW;
    case 95:
    case 96:
    case 99:
        return WEATHER_ICON_THUNDER;
    default:
        return WEATHER_ICON_UNKNOWN;
    }
}
