package com.mintdesk.client

object NativeGns {
    init {
        System.loadLibrary("mintdesk_native")
    }

    external fun selfTest(): String

    external fun clientHello(host: String, port: Int): String
}
