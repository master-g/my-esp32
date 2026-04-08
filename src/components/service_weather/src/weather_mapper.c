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

uint8_t weather_mapper_icon_for_code(uint16_t weather_code, bool is_day)
{
    (void)is_day;

    switch (weather_code) {
    case 0:
        return 1;
    case 1:
    case 2:
    case 3:
        return 2;
    case 45:
    case 48:
        return 3;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
    case 80:
    case 81:
    case 82:
        return 4;
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return 5;
    case 95:
    case 96:
    case 99:
        return 6;
    default:
        return 0;
    }
}
