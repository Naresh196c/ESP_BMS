#include "../bms_gatt.h"
#include <algorithm>
#include <stdio.h>
int criticalDepth=0;std::vector<int> calls;std::vector<uint8_t> command;GattHandler BLEDevice::handler=nullptr;
static int count(int kind){return std::count(calls.begin(),calls.end(),kind);}
static void deliver(esp_gattc_cb_event_t event,esp_ble_gattc_cb_param_t p){BLEDevice::handler(event,3,&p);assert(criticalDepth==0);}
int main(){
 BmsGatt g;uint8_t peer[]={1,2,3,4,5,6};BLEAddress address(peer);
 assert(g.begin()==ESP_OK);assert(count(REGISTER)==1);
 esp_ble_gattc_cb_param_t p={};p.reg.app_id=0x424D;p.reg.status=ESP_GATT_OK;deliver(ESP_GATTC_REG_EVT,p);
 assert(g.open(address,0)==ESP_OK);assert(g.open(address,0)==ESP_ERR_INVALID_STATE);assert(count(OPEN)==1);
 p={};memcpy(p.connect.remote_bda,peer,6);p.connect.conn_id=7;deliver(ESP_GATTC_CONNECT_EVT,p);assert(count(MTU)==1);
 p={};p.dis_srvc_cmpl.conn_id=7;p.dis_srvc_cmpl.status=0;deliver(ESP_GATTC_DIS_SRVC_CMPL_EVT,p);
 p={};memcpy(p.open.remote_bda,peer,6);p.open.conn_id=7;p.open.status=0;deliver(ESP_GATTC_OPEN_EVT,p);
 assert(g.snapshot().discoveryDone);assert(g.search()==0);
 p={};p.search_res.conn_id=7;p.search_res.start_handle=10;p.search_res.end_handle=30;p.search_res.srvc_id.uuid.len=2;p.search_res.srvc_id.uuid.uuid.uuid16=0xFFE0;deliver(ESP_GATTC_SEARCH_RES_EVT,p);
 p={};p.search_cmpl.conn_id=7;p.search_cmpl.status=0;deliver(ESP_GATTC_SEARCH_CMPL_EVT,p);
 assert(g.findCharacteristic()==0);assert(g.registerNotifications()==0);
 p={};p.reg_for_notify.handle=99;deliver(ESP_GATTC_REG_FOR_NOTIFY_EVT,p);assert(!g.snapshot().notifyDone);
 p.reg_for_notify.handle=12;deliver(ESP_GATTC_REG_FOR_NOTIFY_EVT,p);assert(g.snapshot().notifyDone);
 assert(g.enableNotifications()==0);
 p={};p.write.conn_id=7;p.write.handle=13;p.write.status=0;deliver(ESP_GATTC_WRITE_DESCR_EVT,p);assert(g.snapshot().descriptorDone);
 assert(g.requestStatus()==0);const uint8_t expected[]={0xEA,0xD1,1,4,0xFF,4,0xFF,0xF5};assert(command==std::vector<uint8_t>(expected,expected+8));
 uint8_t f[54]={0xEA,0xD1,1,50,0xFF,4};f[7]=42;f[47]=0x14;f[48]=0x93;f[53]=0xF5;for(int i=3;i<52;++i)f[52]^=f[i];
 p={};p.notify.conn_id=99;p.notify.handle=12;memcpy(p.notify.remote_bda,peer,6);p.notify.value=f;p.notify.value_len=20;deliver(ESP_GATTC_NOTIFY_EVT,p);assert(g.snapshot().rx.bytes==0);
 p.notify.conn_id=7;deliver(ESP_GATTC_NOTIFY_EVT,p);p.notify.value=f+20;p.notify.value_len=34;deliver(ESP_GATTC_NOTIFY_EVT,p);assert(g.snapshot().rx.ready);
 assert(g.close()==0);assert(count(DISCONNECT)==1);assert(!g.snapshot().link.idle());assert(g.open(address,0)==ESP_ERR_INVALID_STATE);
 // A virtual CLOSE is diagnostic only. Even a long delay must not release
 // the physical slot; the eventual DISCONNECT permits progress without reset.
 p={};memcpy(p.close.remote_bda,peer,6);p.close.conn_id=7;p.close.status=0;deliver(ESP_GATTC_CLOSE_EVT,p);
 assert(g.snapshot().closeSeen&&g.snapshot().closeStatus==0);
 for(int i=0;i<1000;++i){assert(!g.snapshot().link.idle());assert(g.open(address,0)==ESP_ERR_INVALID_STATE);}
 assert(count(OPEN)==1&&count(DISCONNECT)==1);
 p={};memcpy(p.disconnect.remote_bda,peer,6);p.disconnect.conn_id=7;deliver(ESP_GATTC_DISCONNECT_EVT,p);assert(g.snapshot().link.idle());
 // Deadline during OPEN: keep registration and reject overlap. A late success
 // must disconnect once, without an MTU exchange, before a new peer can open.
 assert(g.open(address,0)==0);assert(g.close()==0);assert(count(DISCONNECT)==1);assert(g.open(address,0)==ESP_ERR_INVALID_STATE);
 p={};memcpy(p.connect.remote_bda,peer,6);p.connect.conn_id=8;deliver(ESP_GATTC_CONNECT_EVT,p);assert(count(DISCONNECT)==2&&count(MTU)==1);
 p={};memcpy(p.open.remote_bda,peer,6);p.open.conn_id=8;deliver(ESP_GATTC_OPEN_EVT,p);assert(count(DISCONNECT)==2);assert(!g.snapshot().link.idle());
 p={};memcpy(p.disconnect.remote_bda,peer,6);p.disconnect.conn_id=8;deliver(ESP_GATTC_DISCONNECT_EVT,p);assert(g.snapshot().link.idle());
 assert(g.open(address,0)==0);p={};memcpy(p.open.remote_bda,peer,6);p.open.status=0x3E;deliver(ESP_GATTC_OPEN_EVT,p);assert(g.snapshot().openStatus==0x3E&&g.snapshot().link.idle());
 assert(g.open(address,0)==0);assert(count(REGISTER)==1);
 puts("PASS: persistent registration, bounded-state callback flow, FFE0/FFE1/CCCD setup, verified FF04 command, notification identity filtering, late OPEN/DISCONNECT cleanup, CLOSE cannot release physical slot, refusal to reuse pending slot, and zero SDK calls inside critical sections.");
}
