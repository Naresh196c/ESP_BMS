#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <vector>
typedef int esp_err_t;typedef uint16_t esp_gatt_status_t;typedef uint16_t esp_gatt_if_t;typedef int esp_ble_addr_type_t;
const int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=0x103;
const uint16_t ESP_GATT_IF_NONE=0xFF,ESP_GATT_OK=0,ESP_GATT_NOT_FOUND=10,ESP_UUID_LEN_16=2;
const uint8_t ESP_GATT_CHAR_PROP_BIT_NOTIFY=16,ESP_GATT_CHAR_PROP_BIT_WRITE=8;
const int ESP_GATT_WRITE_TYPE_RSP=1,ESP_GATT_AUTH_REQ_NONE=0;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
extern int criticalDepth;
#define portENTER_CRITICAL(x) do { (void)(x);++criticalDepth; } while(0)
#define portEXIT_CRITICAL(x) do { (void)(x);--criticalDepth; } while(0)
struct BLEAddress {uint8_t value[6];explicit BLEAddress(const uint8_t* p){memcpy(value,p,6);}uint8_t* getNative(){return value;}};
struct esp_bt_uuid_t {uint16_t len;struct {uint16_t uuid16;}uuid;};
struct esp_gattc_char_elem_t {uint16_t char_handle;uint8_t properties;};
struct esp_gattc_descr_elem_t {uint16_t handle;};
enum esp_gattc_cb_event_t {ESP_GATTC_REG_EVT,ESP_GATTC_CONNECT_EVT,ESP_GATTC_OPEN_EVT,ESP_GATTC_DISCONNECT_EVT,ESP_GATTC_CLOSE_EVT,ESP_GATTC_DIS_SRVC_CMPL_EVT,ESP_GATTC_SEARCH_RES_EVT,ESP_GATTC_SEARCH_CMPL_EVT,ESP_GATTC_REG_FOR_NOTIFY_EVT,ESP_GATTC_WRITE_DESCR_EVT,ESP_GATTC_WRITE_CHAR_EVT,ESP_GATTC_NOTIFY_EVT};
struct esp_ble_gattc_cb_param_t {
 struct {uint16_t app_id,status;}reg;
 struct {uint8_t remote_bda[6];uint16_t conn_id;}connect;
 struct {uint8_t remote_bda[6];uint16_t conn_id,status;}open;
 struct {uint8_t remote_bda[6];uint16_t conn_id,reason;}disconnect;
 struct {uint8_t remote_bda[6];uint16_t conn_id,status;}close;
 struct {uint16_t conn_id,status;}dis_srvc_cmpl;
 struct {uint16_t conn_id,start_handle,end_handle;struct {esp_bt_uuid_t uuid;}srvc_id;}search_res;
 struct {uint16_t conn_id,status;}search_cmpl;
 struct {uint16_t handle,status;}reg_for_notify;
 struct {uint16_t conn_id,handle,status;}write;
 struct {uint16_t conn_id,handle;uint8_t remote_bda[6];uint8_t* value;uint16_t value_len;}notify;
};
typedef void(*GattHandler)(esp_gattc_cb_event_t,esp_gatt_if_t,esp_ble_gattc_cb_param_t*);
struct BLEDevice {static GattHandler handler;static void setCustomGattcHandler(GattHandler h){handler=h;}};
extern std::vector<int> calls;extern std::vector<uint8_t> command;
enum {REGISTER,OPEN,MTU,SEARCH,CHAR_LOOKUP,NOTIFY_REGISTER,DESCR_LOOKUP,DESCR_WRITE,CHAR_WRITE,NOTIFY_UNREGISTER,DISCONNECT};
inline int record(int call){assert(criticalDepth==0);calls.push_back(call);return ESP_OK;}
inline esp_err_t esp_ble_gattc_app_register(uint16_t id){assert(id==0x424D);return record(REGISTER);}
inline esp_err_t esp_ble_gattc_open(esp_gatt_if_t,uint8_t*,esp_ble_addr_type_t,bool){return record(OPEN);}
inline esp_err_t esp_ble_gattc_send_mtu_req(esp_gatt_if_t,uint16_t){return record(MTU);}
inline esp_err_t esp_ble_gap_disconnect(uint8_t*){return record(DISCONNECT);}
inline esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t,uint16_t,esp_bt_uuid_t*){return record(SEARCH);}
inline esp_gatt_status_t esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t,uint16_t,uint16_t,uint16_t,esp_bt_uuid_t,esp_gattc_char_elem_t* out,uint16_t* count){record(CHAR_LOOKUP);*count=1;out->char_handle=12;out->properties=24;return ESP_GATT_OK;}
inline esp_err_t esp_ble_gattc_register_for_notify(esp_gatt_if_t,uint8_t*,uint16_t){return record(NOTIFY_REGISTER);}
inline esp_gatt_status_t esp_ble_gattc_get_descr_by_char_handle(esp_gatt_if_t,uint16_t,uint16_t,esp_bt_uuid_t,esp_gattc_descr_elem_t* out,uint16_t* count){record(DESCR_LOOKUP);*count=1;out->handle=13;return ESP_GATT_OK;}
inline esp_err_t esp_ble_gattc_write_char_descr(esp_gatt_if_t,uint16_t,uint16_t,uint16_t n,uint8_t* p,int,int){assert(n==2&&p[0]==1&&p[1]==0);return record(DESCR_WRITE);}
inline esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t,uint16_t,uint16_t,uint16_t n,uint8_t* p,int,int){command.assign(p,p+n);return record(CHAR_WRITE);}
inline esp_err_t esp_ble_gattc_unregister_for_notify(esp_gatt_if_t,uint8_t*,uint16_t){return record(NOTIFY_UNREGISTER);}
