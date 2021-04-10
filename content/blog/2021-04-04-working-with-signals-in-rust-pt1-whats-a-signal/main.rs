use std::time::Duration;

extern "C" fn handle_interrupt(_sig: libc::c_int) {
    println!("Sorry we didn't get the chance to finish");
}

fn main() {
    println!("Hello");
    unsafe { 
        libc::signal(libc::SIGINT, handle_interrupt as libc::sighandler_t); 
    }

    std::thread::sleep(Duration::from_secs(10)); 
    println!("Goodbye");
}