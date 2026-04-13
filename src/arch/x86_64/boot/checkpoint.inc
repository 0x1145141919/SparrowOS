; 按照头文件定义的check_point结构体格式
struc check_point
    .success_word:        resd 1   ; uint32_t success_word
    .failure_flags:       resb 1   ; uint8_t failure_flags
    .failure_caused_excption_num: resb 1   ; uint8_t failure_caused_excption_num
    .failure_final_stack_top:     resw 1   ; uint16_t failure_final_stack_top
    .check_point_id:      resw 1   ; uint16_t check_point_id
    .reserved:            resw 3   ; uint16_t reserved[3]
endstruc