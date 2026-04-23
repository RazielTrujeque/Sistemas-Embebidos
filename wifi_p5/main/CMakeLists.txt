idf_component_register(SRCS "softap_example_main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES
                    "index.html"
                    "style.css"
                    "app.js"
                    REQUIRES
                    nvs_flash        
                    esp_wifi         
                    esp_netif       
                    esp_event       
                    esp_http_server  
                    esp_timer        
                    driver    )
