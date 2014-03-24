#include "pebble.h"

static const uint32_t WEATHER_ICONS[] = {
  RESOURCE_ID_CLEAR_DAY,
  RESOURCE_ID_CLEAR_NIGHT,
  RESOURCE_ID_WINDY,
  RESOURCE_ID_COLD,
  RESOURCE_ID_PARTLY_CLOUDY_DAY,
  RESOURCE_ID_PARTLY_CLOUDY_NIGHT,
  RESOURCE_ID_HAZE,
  RESOURCE_ID_CLOUD,
  RESOURCE_ID_RAIN,
  RESOURCE_ID_SNOW,
  RESOURCE_ID_HAIL,
  RESOURCE_ID_CLOUDY,
  RESOURCE_ID_STORM,
  RESOURCE_ID_NA,
};

enum WeatherKey {
  WEATHER_ICON_KEY = 0x0,         // TUPLE_INT
  WEATHER_TEMPERATURE_KEY = 0x1,  // TUPLE_CSTRING
  INVERT_COLOR_KEY = 0x2,  // TUPLE_CSTRING
};

Window *window;
BitmapLayer *icon_layer;
GBitmap *icon_bitmap = NULL;
TextLayer *temp_layer;

TextLayer *text_day_layer;
TextLayer *text_date_layer;
TextLayer *text_time_layer;
TextLayer *text_seconds_layer;
Layer *line_layer;
Layer *battery_layer; //Battery now graphical

InverterLayer *inverter_layer = NULL;

typedef struct State {
  int battery_charging;
  int battery_level;
} State;
static State state = {2, 0};

static AppSync sync;
static uint8_t sync_buffer[64];

void set_invert_color(bool invert) {
  if (invert && inverter_layer == NULL) {
    // Add inverter layer
    Layer *window_layer = window_get_root_layer(window);

    inverter_layer = inverter_layer_create(GRect(0, 0, 144, 168));
    layer_add_child(window_layer, inverter_layer_get_layer(inverter_layer));
  } else if (!invert && inverter_layer != NULL) {
    // Remove Inverter layer
    layer_remove_from_parent(inverter_layer_get_layer(inverter_layer));
    inverter_layer_destroy(inverter_layer);
    inverter_layer = NULL;
  }
  // No action required
}

static void sync_tuple_changed_callback(const uint32_t key,
                                        const Tuple* new_tuple,
                                        const Tuple* old_tuple,
                                        void* context) {
  bool invert;

  // App Sync keeps new_tuple in sync_buffer, so we may use it directly
  switch (key) {
    case WEATHER_ICON_KEY:
      if (icon_bitmap) {
        gbitmap_destroy(icon_bitmap);
      }

      icon_bitmap = gbitmap_create_with_resource(
          WEATHER_ICONS[new_tuple->value->uint8]);
      bitmap_layer_set_bitmap(icon_layer, icon_bitmap);
      break;

    case WEATHER_TEMPERATURE_KEY:
      text_layer_set_text(temp_layer, new_tuple->value->cstring);
      break;

    case INVERT_COLOR_KEY:
      invert = new_tuple->value->uint8 != 0;
      persist_write_bool(INVERT_COLOR_KEY, invert);
      set_invert_color(invert);
      break;
  }
}

// Redraw line between date and time
void line_layer_update_callback(Layer *layer, GContext* ctx) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  
  graphics_draw_rect(ctx, GRect(8, 3, 144-39, 2));
  //also draws colon for seconds, this way it is centered
  graphics_draw_rect(ctx, GRect(144-35+8, 0, 2, 2));
  graphics_draw_rect(ctx, GRect(144-35+8, 6, 2, 2));
  //graphics_context_set_fill_color(ctx, GColorWhite);
  //graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

void bluetooth_connection_changed(bool connected) {
  static bool _connected = true;

  // This seemed to get called twice on disconnect
  if (!connected && _connected) {
    vibes_double_pulse(); //now double pulse on disconnect

    if (icon_bitmap) {
      gbitmap_destroy(icon_bitmap);
    }

    icon_bitmap = gbitmap_create_with_resource(RESOURCE_ID_NO_BT);
    bitmap_layer_set_bitmap(icon_layer, icon_bitmap);
  }
  _connected = connected;
}

void handle_tick(struct tm *tick_time, TimeUnits units_changed) {
  // Need to be static because they're used by the system later.
  static char day_text[] = "xxxxxxxxx";
  static char date_text[] = "Xxxxxxxxx 00";
  static char time_text[] = "00:00";
  static char seconds_text[] = "00"; //added seconds timer
  static int yday = -1;
  static int min = -1;
  static const uint32_t const segments[] = {60};
  VibePattern tinytick = { //pattern for small vibration
    .durations = segments,
    .num_segments = 1,
  };

  char *time_format;

  // Only update the date when it has changed.
  if (yday != tick_time->tm_yday) {
    strftime(day_text, sizeof(day_text), "%A", tick_time);
    text_layer_set_text(text_day_layer, day_text);

    strftime(date_text, sizeof(date_text), "%B %e", tick_time);
    text_layer_set_text(text_date_layer, date_text);
  }
    
  if (clock_is_24h_style()) {
    time_format = "%R";
  } else {
    time_format = "%I:%M";
  }
  
  if ((tick_time->tm_sec == 0) && (tick_time->tm_min % 30 == 0))
    vibes_enqueue_custom_pattern(tinytick); //vibrates slightly on the half hour
  
  if (min != tick_time->tm_min) {
    strftime(time_text, sizeof(time_text), time_format, tick_time);
    text_layer_set_text(text_time_layer, time_text);
    
    // Handle lack of non-padded hour format string for twelve hour clock.
    if (!clock_is_24h_style() && (time_text[0] == '0')) {
      text_layer_set_text(text_time_layer, time_text + 1);
    } else {
      text_layer_set_text(text_time_layer, time_text);
    }
  }

  strftime(seconds_text, sizeof(seconds_text), "%S", tick_time);
  text_layer_set_text(text_seconds_layer, seconds_text);
}

// FIXME testing code
void update_battery_state(BatteryChargeState battery_state) {
  layer_mark_dirty(battery_layer);
  state.battery_charging = battery_state.is_charging;
  state.battery_level = battery_state.charge_percent;
}

void battery_layer_update_callback(Layer *layer, GContext* ctx) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  
  //graphically represent battery state
  //only shows information you "need to know" when you need to know it
  //encourages use of 40-80 rule of li-ion batteries
  if (state.battery_charging == 0) {
    //20-40 draining, display small flag in corner
    if (state.battery_level <= 40) {
      graphics_draw_line(ctx, GPoint(16,0), GPoint(16,0));
      graphics_draw_line(ctx, GPoint(15,0), GPoint(16,1));
      graphics_draw_line(ctx, GPoint(14,0), GPoint(16,2));
      graphics_draw_line(ctx, GPoint(13,0), GPoint(16,3));
      graphics_draw_line(ctx, GPoint(12,0), GPoint(16,4));
      graphics_draw_line(ctx, GPoint(11,0), GPoint(16,5));
      graphics_draw_line(ctx, GPoint(10,0), GPoint(16,6));
      graphics_draw_line(ctx, GPoint(9,0), GPoint(16,7));
    }
    //0-20 draining, display large flag in corner
    if (state.battery_level <= 20) {
      graphics_draw_line(ctx, GPoint(8,0), GPoint(16,8));
      graphics_draw_line(ctx, GPoint(7,0), GPoint(16,9));
      graphics_draw_line(ctx, GPoint(6,0), GPoint(16,10));
      graphics_draw_line(ctx, GPoint(5,0), GPoint(16,11));
      graphics_draw_line(ctx, GPoint(4,0), GPoint(16,12));
      graphics_draw_line(ctx, GPoint(3,0), GPoint(16,13));
      graphics_draw_line(ctx, GPoint(2,0), GPoint(16,14));
      graphics_draw_line(ctx, GPoint(1,0), GPoint(16,15));
    }
  }
  
  //80-100 charging, display stripe flag in corner
  if ((state.battery_charging == 1) && (state.battery_level >= 80)) {
    graphics_draw_line(ctx, GPoint(4,0), GPoint(16,12));
    graphics_draw_line(ctx, GPoint(3,0), GPoint(16,13));
    graphics_draw_line(ctx, GPoint(2,0), GPoint(16,14));
    graphics_draw_line(ctx, GPoint(1,0), GPoint(16,15));
  }
}

void handle_init(void) {
  window = window_create();
  window_stack_push(window, true /* Animated */);
  window_set_background_color(window, GColorBlack);

  Layer *window_layer = window_get_root_layer(window);

  // Setup weather bar
  Layer *weather_holder = layer_create(GRect(0, 0 + 4, 144, 50));
  layer_add_child(window_layer, weather_holder);

  //shifted a lot of things
  
  //icon_layer = bitmap_layer_create(GRect(0, 0, 40, 40));
  //icon_layer = bitmap_layer_create(GRect(1, 0 + 4, 40, 40));
  icon_layer = bitmap_layer_create(GRect(4, 0 + 4, 40, 40));
  layer_add_child(weather_holder, bitmap_layer_get_layer(icon_layer));

  //temp_layer = text_layer_create(GRect(40, 3, 144 - 40, 28));
  //temp_layer = text_layer_create(GRect(39, 1, 144 - 40, 28));
  //temp_layer = text_layer_create(GRect(41, 1, 144 - 40, 28));
  temp_layer = text_layer_create(GRect(43 + 4, 2 + 4, 144 - 40, 28));
  text_layer_set_text_color(temp_layer, GColorWhite);
  text_layer_set_background_color(temp_layer, GColorClear);
  text_layer_set_font(temp_layer,
      fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(temp_layer, GTextAlignmentLeft);
  layer_add_child(weather_holder, text_layer_get_layer(temp_layer));

  // Initialize date & time text
  //Layer *date_holder = layer_create(GRect(0, 52, 144, 94));
  Layer *date_holder = layer_create(GRect(0, 52 + 3, 144, 96 + 3));
  layer_add_child(window_layer, date_holder);

  ResHandle roboto_21 = resource_get_handle(RESOURCE_ID_FONT_ROBOTO_CONDENSED_21);
  text_day_layer = text_layer_create(GRect(8, 0 + 3, 144-8, 25));
  text_layer_set_text_color(text_day_layer, GColorWhite);
  text_layer_set_background_color(text_day_layer, GColorClear);
  text_layer_set_font(text_day_layer, fonts_load_custom_font(roboto_21));
  layer_add_child(date_holder, text_layer_get_layer(text_day_layer));

  text_date_layer = text_layer_create(GRect(8, 21 + 3, 144-8, 25));
  text_layer_set_text_color(text_date_layer, GColorWhite);
  text_layer_set_background_color(text_date_layer, GColorClear);
  text_layer_set_font(text_date_layer, fonts_load_custom_font(roboto_21));
  layer_add_child(date_holder, text_layer_get_layer(text_date_layer));

  //reduced length to accomodate seconds timer
  //line_layer = layer_create(GRect(8, 51, 144-16, 2));
  line_layer = layer_create(GRect(0, 51 + 3 - 3, 144, 2 + 6));
  layer_set_update_proc(line_layer, line_layer_update_callback);
  layer_add_child(date_holder, line_layer);

  ResHandle roboto_49 = resource_get_handle(RESOURCE_ID_FONT_ROBOTO_BOLD_SUBSET_49);
  //text_time_layer = text_layer_create(GRect(7, 45, 144-7, 49));
  text_time_layer = text_layer_create(GRect(7, 47 + 3, 144-7, 49));
  text_layer_set_text_color(text_time_layer, GColorWhite);
  text_layer_set_background_color(text_time_layer, GColorClear);
  text_layer_set_font(text_time_layer, fonts_load_custom_font(roboto_49));
  layer_add_child(date_holder, text_layer_get_layer(text_time_layer));
  
  //added seconds timer
  //text_seconds_layer = text_layer_create(GRect(0, 168 - 18, 144, 168));
  //text_seconds_layer = text_layer_create(GRect(0, 76, 144-8, 18));
  text_seconds_layer = text_layer_create(GRect(0, 90 + 7, 144-8, 18));
  text_layer_set_text_color(text_seconds_layer, GColorWhite);
  text_layer_set_background_color(text_seconds_layer, GColorClear);
  text_layer_set_font(text_seconds_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(text_seconds_layer, GTextAlignmentRight);
  layer_add_child(window_layer, text_layer_get_layer(text_seconds_layer));

  // Setup messaging
  const int inbound_size = 64;
  const int outbound_size = 64;
  app_message_open(inbound_size, outbound_size);

  Tuplet initial_values[] = {
    TupletInteger(WEATHER_ICON_KEY, (uint8_t) 13),
    TupletCString(WEATHER_TEMPERATURE_KEY, ""),
    TupletInteger(INVERT_COLOR_KEY, persist_read_bool(INVERT_COLOR_KEY)),
  };

  app_sync_init(&sync, sync_buffer, sizeof(sync_buffer), initial_values,
                ARRAY_LENGTH(initial_values), sync_tuple_changed_callback,
                NULL, NULL);

  battery_layer = layer_create(GRect(144-16, 0, 16, 16));
  layer_set_update_proc(battery_layer, battery_layer_update_callback);
  layer_add_child(window_layer, battery_layer);

  // Subscribe to notifications
  bluetooth_connection_service_subscribe(bluetooth_connection_changed);
  tick_timer_service_subscribe(SECOND_UNIT, handle_tick); //changed to seconds
  battery_state_service_subscribe(update_battery_state);

  // Update the battery on launch
  update_battery_state(battery_state_service_peek());

  // TODO: Update display here to avoid blank display on launch?
}

void handle_deinit(void) {
  bluetooth_connection_service_unsubscribe();
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
}

int main(void) {
  handle_init();

  app_event_loop();

  handle_deinit();
}
