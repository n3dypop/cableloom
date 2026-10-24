#![allow(non_snake_case)]
use crate::win::{self,Handle,wide};
use std::{ffi::c_void,mem::{size_of,zeroed},ptr::{null,null_mut}};
type WndProc=unsafe extern "system" fn(Handle,u32,usize,isize)->isize;
#[repr(C)]
struct WindowClass { style:u32, procedure:Option<WndProc>, class_extra:i32, window_extra:i32, instance:Handle, icon:Handle, cursor:Handle, background:Handle, menu:*const u16, name:*const u16 }
#[repr(C)]
struct Point {x:i32,y:i32}
#[repr(C)]
struct Message {window:Handle,message:u32,wparam:usize,lparam:isize,time:u32,point:Point,private:u32}
#[repr(C)]
struct Rect {left:i32,top:i32,right:i32,bottom:i32}
#[link(name="user32")]
extern "system" {
    fn RegisterClassW(class:*const WindowClass)->u16;
    fn CreateWindowExW(ex:u32,class:*const u16,text:*const u16,style:u32,x:i32,y:i32,width:i32,height:i32,parent:Handle,menu:Handle,instance:Handle,param:*mut c_void)->Handle;
    fn DefWindowProcW(window:Handle,message:u32,wparam:usize,lparam:isize)->isize;
    fn DestroyWindow(window:Handle)->i32;
    fn PostQuitMessage(code:i32);
    fn GetMessageW(message:*mut Message,window:Handle,min:u32,max:u32)->i32;
    fn TranslateMessage(message:*const Message)->i32;
    fn DispatchMessageW(message:*const Message)->isize;
    fn IsDialogMessageW(window:Handle,message:*mut Message)->i32;
    fn ShowWindow(window:Handle,show:i32)->i32;
    fn GetDlgItem(window:Handle,id:i32)->Handle;
    fn GetWindowTextW(window:Handle,text:*mut u16,count:i32)->i32;
    fn SetWindowTextW(window:Handle,text:*const u16)->i32;
    fn MessageBoxW(window:Handle,text:*const u16,title:*const u16,kind:u32)->i32;
    fn LoadCursorW(instance:Handle,name:*const u16)->Handle;
    fn SendMessageW(window:Handle,message:u32,wparam:usize,lparam:isize)->isize;
    fn MoveWindow(window:Handle,x:i32,y:i32,width:i32,height:i32,repaint:i32)->i32;
    fn GetClientRect(window:Handle,rect:*mut Rect)->i32;
}
#[link(name="kernel32")]
extern "system" {fn GetModuleHandleW(name:*const u16)->Handle;}
#[link(name="gdi32")]
extern "system" {fn GetStockObject(index:i32)->Handle;}
const ID:i32=10; const DELAY:i32=11; const INF:i32=12; const STATUS:i32=13;
pub fn message(text:&str,error:bool) {unsafe{MessageBoxW(null_mut(),wide(text).as_ptr(),wide("Cableloom").as_ptr(),if error{0x10}else{0x40});}}
unsafe fn child(parent:Handle,class:&str,text:&str,style:u32,x:i32,y:i32,w:i32,h:i32,id:i32)->Handle {
    let control=CreateWindowExW(if class=="EDIT"{0x200}else{0},wide(class).as_ptr(),wide(text).as_ptr(),
        0x50000000|style,x,y,w,h,parent,id as usize as Handle,GetModuleHandleW(null()),null_mut());
    SendMessageW(control,0x30,GetStockObject(17) as usize,1);
    control
}
unsafe fn input(window:Handle,id:i32)->String {
    let mut text=vec![0u16;32768];
    let n=GetWindowTextW(GetDlgItem(window,id),text.as_mut_ptr(),text.len() as i32);
    String::from_utf16_lossy(&text[..n.max(0) as usize])
}
unsafe fn refresh(window:Handle) {
    let result=win::devices().map(|all|{
        if all.is_empty(){return "No active cable interfaces.\r\n\r\nInstall a signed driver package and add a cable.\r\n\r\n".to_string()+&win::diagnose().unwrap_or_else(|e|e);}
        all.into_iter().map(|d|match d.status{Ok(s)=>s.line(),Err(e)=>format!("{}\r\n{e}",d.path)}).collect::<Vec<_>>().join("\r\n\r\n")
    }).unwrap_or_else(|e|e);
    SetWindowTextW(GetDlgItem(window,STATUS),wide(&result).as_ptr());
}
unsafe extern "system" fn procedure(window:Handle,msg:u32,wparam:usize,lparam:isize)->isize {
    match msg {
        1=>{
            child(window,"STATIC","Cable ID (1-8)",0,16,16,110,22,0);
            child(window,"EDIT","1",0x10000|0x2000,130,12,60,26,ID);
            child(window,"STATIC","Delay (frames)",0,210,16,110,22,0);
            child(window,"EDIT","96",0x10000|0x2000,324,12,75,26,DELAY);
            for (id,text,x,w) in [(101,"Refresh",16,100),(102,"Add cable",126,110),(103,"Remove cable",246,120),(104,"Set delay",376,110)] {
                child(window,"BUTTON",text,0x10000,x,50,w,30,id);
            }
            child(window,"STATIC","Signed driver INF",0,16,98,130,22,0);
            let path=std::env::current_exe().ok().and_then(|p|p.parent().map(|p|p.join("cableloom.inf"))).map(|p|p.to_string_lossy().into_owned()).unwrap_or_default();
            child(window,"EDIT",&path,0x10000|0x80,16,122,570,28,INF);
            child(window,"BUTTON","Install package",0x10000,596,122,140,28,105);
            child(window,"EDIT","",0x10000|0x4|0x800|0x40|0x200000,16,166,720,270,STATUS);
            child(window,"STATIC","48 kHz stereo PCM16/24/32. Stop both streams before changing delay.\r\nAdd/remove/install request administrator access. Refresh after device changes.",0,16,448,720,46,0);
            if [ID,DELAY,INF,STATUS,101,102,103,104,105].iter().any(|id|GetDlgItem(window,*id).is_null()) {
                message("Cannot create manager controls.",true);return -1;
            }
            refresh(window);0
        },
        5=>{
            let mut rect:Rect=zeroed();GetClientRect(window,&mut rect);
            MoveWindow(GetDlgItem(window,STATUS),16,166,(rect.right-32).max(200),(rect.bottom-240).max(100),1);
            0
        },
        0x111=>{
            if wparam>>16!=0{return DefWindowProcW(window,msg,wparam,lparam);}
            let command=(wparam&0xffff) as i32;
            if command==101 {refresh(window);return 0;}
            if !(102..=105).contains(&command){return DefWindowProcW(window,msg,wparam,lparam);}
            let result=(||->Result<String,String>{
                if command==105 {return win::elevated(&["install".into(),input(window,INF)]);}
                let id=input(window,ID).parse::<u32>().map_err(|_|"Cable ID must be 1..8.")?;
                if !(1..=8).contains(&id){return Err("Cable ID must be 1..8.".into());}
                match command {
                    102=>win::elevated(&["add".into(),id.to_string()]),
                    103=>{
                        if MessageBoxW(window,wide(&format!("Remove Cable {id}? Any streams on this cable will stop.")).as_ptr(),wide("Remove cable").as_ptr(),0x124)!=6 {
                            return Ok("Removal canceled.".into());
                        }
                        win::elevated(&["remove".into(),id.to_string()])
                    },
                    104=>{
                        let frames=input(window,DELAY).parse::<u32>().map_err(|_|"Delay must be 48..480 frames.")?;
                        win::delay(id,frames)?;Ok(format!("Delay set to {frames} frames."))
                    },
                    _=>unreachable!()
                }
            })();
            match result{Ok(text)=>{refresh(window);message(&text,false);},Err(e)=>message(&e,true)}
            0
        },
        0x10=>{DestroyWindow(window);0},
        2=>{PostQuitMessage(0);0},
        _=>DefWindowProcW(window,msg,wparam,lparam)
    }
}
pub fn run()->Result<(),String>{
    unsafe {
        let instance=GetModuleHandleW(null());
        let name=wide("CableloomManager");
        let class=WindowClass {style:3,procedure:Some(procedure),class_extra:0,window_extra:0,instance,
            icon:null_mut(),cursor:LoadCursorW(null_mut(),32512usize as *const u16),background:6usize as Handle,menu:null(),name:name.as_ptr()};
        if RegisterClassW(&class)==0{return Err(format!("Register window: {}",std::io::Error::last_os_error()));}
        // Fixed dimensions keep the controls usable without a layout dependency.
        let window=CreateWindowExW(0,name.as_ptr(),wide("Cableloom - Virtual Audio Cables").as_ptr(),0x00ca0000,
            i32::MIN,i32::MIN,775,550,null_mut(),null_mut(),instance,null_mut());
        if window.is_null(){return Err(format!("Create window: {}",std::io::Error::last_os_error()));}
        ShowWindow(window,1);
        let mut msg:Message=zeroed();
        loop {
            let result=GetMessageW(&mut msg,null_mut(),0,0);
            if result==0{break;}
            if result<0{return Err(format!("Window message: {}",std::io::Error::last_os_error()));}
            if IsDialogMessageW(window,&mut msg)==0{TranslateMessage(&msg);DispatchMessageW(&msg);}
        }
        Ok(())
    }
}

const _: () = assert!(size_of::<WindowClass>() == 72);
const _: () = assert!(size_of::<Message>() == 48);
