//****************************************Copyright (c)***********************************//
//原子哥在线教学平台：www.yuanzige.com
//技术支持：www.openedv.com
//淘宝店铺：http://openedv.taobao.com
//关注微信公众平台微信号："正点原子"，免费获取ZYNQ & FPGA & STM32 & LINUX资料。
//版权所有，盗版必究。
//Copyright(C) 正点原子 2018-2028
//All rights reserved
//----------------------------------------------------------------------------------------
// File name:           ov5640_capture_data
// Last modified Date:  2019/6/21 14:26:05
// Last Version:        V1.0
// Descriptions:       ov5640数据采集模块
//----------------------------------------------------------------------------------------
// Created by:          正点原子
// Created date:        2019/6/21 14:26:05
// Version:             V1.0
// Descriptions:        The original version
//
//----------------------------------------------------------------------------------------
//****************************************************************************************//

module  ov5640_capture_data(
    input                 rst_n           ,  //复位信号

	//摄像头接口
    input                 cam_pclk        ,  //cam 数据像素时钟
    input                 cam_vsync       ,  //cam 场同步信号
    input                 cam_href        ,  //cam 行同步信号
    input        [7:0]    cam_data        ,  //cam 数据
    output                cam_rst_n       ,  //cmos 复位信号，低电平有效
    output                cam_pwdn        ,  //电源休眠模式选择

	//RGB888接口
    output                cmos_frame_clk,    //时钟信号
    output                cmos_frame_ce,     //时钟使能信号

	output                cmos_frame_vsync,  //帧有效信号
    output                cmos_frame_href ,  //行有效信号
    output                cmos_frame_de ,    //数据有效
    output       [23:0]   cmos_frame_data ,  //有效数据

	//ISP enable: bit0 gray, 1 gauss, 2 sharpen, 3 sobel
	input        [3:0]    isp_en
);

//parameter define

//寄存器全部配置完成后，先等待10帧数据
//待寄存器配置生效后再开始采集图像
localparam  WAIT_FRAME = 4'd10  ;            //寄存器数据稳定等待的帧个数

//reg define
reg          rst_n_d0 =1;
reg          rst_n_syn =1;
reg          cam_vsync_d0 ;
reg          cam_vsync_d1 ;
reg          cam_href_d0 ;
reg          cam_href_d1 ;
reg   [3:0]  cmos_ps_cnt ;                  //等待帧数稳定计数器
reg          wait_done ;
reg          byte_flag ;
reg   [7:0]  cam_data_d0 ;
reg  [15:0]  cmos_data_16b ;                //用于8位转16位的临时寄存器
reg          byte_flag_d0 ;

//wire define
wire  pos_vsync ;

//*****************************************************
//**                    main code
//*****************************************************

//采输入场同步信号的上升沿
assign  pos_vsync  = (~cam_vsync_d1) & cam_vsync_d0 ;

//不对摄像头硬件复位,固定高电平
assign  cam_rst_n = 1'b1;

//电源休眠模式选择 0：正常模式 1：电源休眠模式
assign  cam_pwdn  = 1'b0;

assign  cmos_frame_clk   = cam_pclk;

wire        isp_vs, isp_hs, isp_de;
wire [23:0] isp_rgb;
wire        cap_vsync_w, cap_href_w, cap_de_w;
wire [23:0] cap_data_w;
//由于摄像头输入接口的两个时钟周期对应于RGB888输出接口的一个有效时钟周期
//所以要给出数据有效标志即RGB888输出接口的时钟使能信号
assign  cmos_frame_ce    = wait_done  ?  (byte_flag_d0 & cmos_frame_href) || (!cmos_frame_href) : 1'b0;
assign  cap_vsync_w = wait_done  ?  cam_vsync_d1  :  1'b0;
assign  cap_href_w  = wait_done  ?  cam_href_d1   :  1'b0;
assign  cap_de_w    = cap_href_w ;
assign  cap_data_w  = wait_done  ?
	{ cmos_data_16b[15:11],3'd0 , cmos_data_16b[10:5],2'd0 , cmos_data_16b[4:0],3'd0 }
	:  24'd0;

isp_chain #(
    .H_SIZE ( 14'd1280 ),
    .V_SIZE ( 14'd720  )
) u_isp (
    .I_vtc_clk ( cam_pclk   ),
    .I_rst_n   ( rst_n_syn  ),
    .I_en      ( isp_en     ),
    .I_vs      ( cap_vsync_w ),
    .I_hs      ( cap_href_w  ),
    .I_de      ( cap_de_w    ),
    .I_rgb     ( cap_data_w  ),
    .O_vs      ( isp_vs  ),
    .O_hs      ( isp_hs  ),
    .O_de      ( isp_de  ),
    .O_rgb     ( isp_rgb )
);

assign  cmos_frame_vsync = isp_vs;
assign  cmos_frame_href  = isp_hs;
assign  cmos_frame_de    = isp_de;
assign  cmos_frame_data  = isp_rgb;


//复位信号的异步复位、同步释放处理
always @(posedge cam_pclk or negedge rst_n) begin
    if(!rst_n) begin
        rst_n_d0 <= 1'b0;
        rst_n_d0 <= 1'b0;
    end
    else begin
        rst_n_d0  <= 1'b1;
        rst_n_syn <= rst_n_d0;
    end
end

//对行、场同步信号的延迟打拍
always @(posedge cam_pclk or negedge rst_n_syn) begin
    if(!rst_n_syn) begin
        cam_vsync_d0 <= 1'b0;
        cam_vsync_d1 <= 1'b0;

        cam_href_d0 <= 1'b0;
        cam_href_d1 <= 1'b0;
    end
    else begin
        cam_vsync_d0 <= cam_vsync;
        cam_vsync_d1 <= cam_vsync_d0;

        cam_href_d0 <= cam_href;
        cam_href_d1 <= cam_href_d0;
    end
end

//寄存器全部配置完成后，先等待10帧数据
//待寄存器配置生效后再开始采集图像
always @(posedge cam_pclk or negedge rst_n_syn) begin
    if(!rst_n_syn)
        cmos_ps_cnt <= 4'd0;
    else if(pos_vsync && (cmos_ps_cnt < WAIT_FRAME))
        cmos_ps_cnt <= cmos_ps_cnt + 4'd1;  //对帧数进行计数
end

//等待完成后 给出 等待完成信号
always @(posedge cam_pclk or negedge rst_n_syn) begin
    if(!rst_n_syn)
        wait_done <= 1'b0;
    else if((cmos_ps_cnt == WAIT_FRAME) && pos_vsync)
        wait_done <= 1'b1;
end


//8位数据转16位RGB565数据
always @(posedge cam_pclk or negedge rst_n_syn) begin
    if(!rst_n_syn) begin
        cmos_data_16b <= 16'd0;
        cam_data_d0 <= 8'd0;
        byte_flag <= 1'b0;
    end
    else if( cam_href ) begin  //cam 行同步信号
        byte_flag   <= ~byte_flag;
        cam_data_d0 <= cam_data;
        if(byte_flag)
            cmos_data_16b <= {cam_data_d0,cam_data};
    end
    else begin
        byte_flag <= 1'b0;
        cam_data_d0 <= 8'b0;
    end
end

always @(posedge cam_pclk or negedge rst_n_syn) begin
    if(!rst_n_syn)
        byte_flag_d0 <= 1'b0;
    else
        byte_flag_d0 <= byte_flag;
end

endmodule


//======================================================================// >> isp_chain.v
// ISP预处理链总封装: 摄像头RGB888流 → 灰度 → 高斯 → 拉普拉斯锐化 → Sobel → VDMA
// I_en: [0]=灰度 [1]=高斯 [2]=锐化 [3]=Sobel, 每级0=直通
`timescale 1ns / 1ps

module isp_chain #(
    parameter [13:0] H_SIZE = 1280,
    parameter [13:0] V_SIZE = 720
)(
    input  wire        I_vtc_clk,
    input  wire        I_rst_n,
    input  wire [3:0]  I_en,
    input  wire        I_vs,
    input  wire        I_hs,
    input  wire        I_de,
    input  wire [23:0] I_rgb,

    output wire        O_vs,
    output wire        O_hs,
    output wire        O_de,
    output wire [23:0] O_rgb
);

wire        vs1,  hs1,  de1;
wire [23:0] rgb1;
wire        vs2,  hs2,  de2;
wire [23:0] rgb2;
wire        vs3,  hs3,  de3;
wire [23:0] rgb3;

rgb2gray #(
    .H_SIZE (H_SIZE),
    .V_SIZE (V_SIZE)
) u_gray (
    .I_vtc_clk (I_vtc_clk),
    .I_rst_n   (I_rst_n),
    .I_en      (I_en[0]),
    .I_vs      (I_vs),
    .I_hs      (I_hs),
    .I_de      (I_de),
    .I_rgb     (I_rgb),
    .O_vs      (vs1),
    .O_hs      (hs1),
    .O_de      (de1),
    .O_rgb     (rgb1)
);

gaussian_filter #(
    .H_SIZE (H_SIZE),
    .V_SIZE (V_SIZE)
) u_gauss (
    .I_vtc_clk (I_vtc_clk),
    .I_rst_n   (I_rst_n),
    .I_en      (I_en[1]),
    .I_vs      (vs1),
    .I_hs      (hs1),
    .I_de      (de1),
    .I_rgb     (rgb1),
    .O_vs      (vs2),
    .O_hs      (hs2),
    .O_de      (de2),
    .O_rgb     (rgb2)
);

laplacian_sharpen #(
    .H_SIZE (H_SIZE),
    .V_SIZE (V_SIZE)
) u_lap (
    .I_vtc_clk (I_vtc_clk),
    .I_rst_n   (I_rst_n),
    .I_en      (I_en[2]),
    .I_vs      (vs2),
    .I_hs      (hs2),
    .I_de      (de2),
    .I_rgb     (rgb2),
    .O_vs      (vs3),
    .O_hs      (hs3),
    .O_de      (de3),
    .O_rgb     (rgb3)
);

sobel #(
    .H_SIZE (H_SIZE),
    .V_SIZE (V_SIZE)
) u_sobel (
    .I_vtc_clk (I_vtc_clk),
    .I_rst_n   (I_rst_n),
    .I_en      (I_en[3]),
    .I_vs      (vs3),
    .I_hs      (hs3),
    .I_de      (de3),
    .I_rgb     (rgb3),
    .O_vs      (O_vs),
    .O_hs      (O_hs),
    .O_de      (O_de),
    .O_rgb     (O_rgb)
);

endmodule

//======================================================================// >> rgb2gray.v
`timescale 1ns / 1ps
//****************************************VSCODE PLUG-IN**********************************//
//----------------------------------------------------------------------------------------
// IDE :                   VSCODE     
// VSCODE plug-in version: Verilog-Hdl-Format-2.8.20240817
// VSCODE plug-in author : Jiang Percy
//----------------------------------------------------------------------------------------
//****************************************Copyright (c)***********************************//
// Copyright(C)            Please Write Company name
// All rights reserved     
// File name:              
// Last modified Date:     2024/11/02 20:32:01
// Last Version:           V1.0
// Descriptions:           
//----------------------------------------------------------------------------------------
// Created by:             Please Write You Name 
// Created date:           2024/11/02 20:32:01
// mail      :             Please Write mail 
// Version:                V1.0
// TEXT NAME:              rgb2gray.v
// PATH:                   C:\Users\Administrator\Desktop\isp_test\uisrc\01_rtl\isp\isp_rgb2gray\isp_rgb2gray.v
// Descriptions:           
//                         
//----------------------------------------------------------------------------------------
//****************************************************************************************//

module rgb2gray#(
parameter   [13:0]  H_SIZE = 1920,    //640*480
parameter   [13:0]  V_SIZE = 1080
)
(
    input   wire        I_vtc_clk,            //cmos video pixel clock
    input   wire        I_rst_n,          //global reset
    input   wire        I_en,             //使能: 1=灰度处理, 0=直通(寄存一级)
    input   wire        I_hs,          //Prepared Image data hsync valid signal
    input   wire        I_vs,          //Prepared Image data vsync valid signal
    input   wire        I_de,          //Prepared Image data href vaild  signal
    input   wire[23:0]  I_rgb,        //Prepared Image data 8 Bit RAW Data


    output  wire        O_hs,   //Processed Image data hsync valid signal
    output  wire        O_vs,   //Processed Image data vsync valid signal
    output  wire        O_de,    //Processed Image data href vaild  signal
    output  wire[23:0]  O_rgb
);
// 使能同步: 仅在I_vs为1时才允许切换，避免帧中间切换导致错位
reg en_sync;
always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n)
        en_sync <= 1'b0;
    else if (I_vs)
        en_sync <= I_en;
end

reg [15:0] R_mult;
reg [15:0] G_mult;
reg [15:0] B_mult;
reg [17:0] gray_temp;

always @(posedge I_vtc_clk or negedge I_rst_n)           
    begin                                        
        if(!I_rst_n) begin
            R_mult <= 16'b0; 
            G_mult <= 16'b0;
            B_mult <= 16'b0;
            gray_temp <= 18'b0;                
        end
        else if (!en_sync) begin  // 禁用时复位处理路径
            R_mult <= 16'b0;
            G_mult <= 16'b0;
            B_mult <= 16'b0;
            gray_temp <= 18'b0;
        end
        else begin
            R_mult <= I_rgb[23:16]*76;
            G_mult <= I_rgb[15:8]*150;
            B_mult <= I_rgb[7:0]*30;
            gray_temp <= R_mult + G_mult + B_mult;
        end                                          
    end     

localparam DLY_CLK = 2;
reg [DLY_CLK-1:0] hs_dly;
reg [DLY_CLK-1:0] vs_dly;
reg [DLY_CLK-1:0] de_dly;

always @ (posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        hs_dly <= 0;
        vs_dly <= 0;
        de_dly <= 0;
    end
    else if (!en_sync) begin  // 禁用时复位延迟链
        hs_dly <= 0;
        vs_dly <= 0;
        de_dly <= 0;
    end
    else begin
        hs_dly <= {hs_dly[DLY_CLK-2:0], I_hs};
        vs_dly <= {vs_dly[DLY_CLK-2:0], I_vs};
        de_dly <= {de_dly[DLY_CLK-2:0], I_de};
    end
end

wire [7:0] gray_out = (gray_temp[17:8] > 255) ? 8'hff : gray_temp[15:8];

// 直通寄存(一级)
reg        bypass_hs ;
reg        bypass_vs ;
reg        bypass_de ;
reg [23:0] bypass_rgb;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        bypass_hs  <= 1'b0;
        bypass_vs  <= 1'b0;
        bypass_de  <= 1'b0;
        bypass_rgb <= 24'd0;
    end else begin
        bypass_hs  <= I_hs;
        bypass_vs  <= I_vs;
        bypass_de  <= I_de;
        bypass_rgb <= I_rgb;
    end
end

// MUX输出: 使能时选灰度，否则选直通
assign O_hs  = en_sync ? hs_dly[DLY_CLK-1]   : bypass_hs ;
assign O_vs  = en_sync ? vs_dly[DLY_CLK-1]   : bypass_vs ;
assign O_de  = en_sync ? de_dly[DLY_CLK-1]    : bypass_de ;
assign O_rgb = en_sync ? {3{gray_out}}         : bypass_rgb;
endmodule
//======================================================================// >> gaussian_filter.v
`timescale 1ns/1ns

module gaussian_filter#(
parameter   [13:0]  H_SIZE = 1920,
parameter   [13:0]  V_SIZE = 1080
)
(
    //global clock
input   wire        I_vtc_clk,      //cmos video pixel clock
input   wire        I_rst_n,        //global reset
input   wire        I_en,           //使能: 1=高斯滤波, 0=直通(寄存一级)
input   wire        I_vs,           //Prepared Image data vsync valid signal
input   wire        I_hs,           //Prepared Image data hsync valid signal
input   wire        I_de,           //Prepared Image data href vaild  signal
input   wire[23:0]  I_rgb,          //Prepared Image data 24 Bit RGB Data

output  wire        O_vs,           //Processed Image data vsync valid signal
output  wire        O_hs,           //Processed Image data hsync valid signal
output  wire        O_de,           //Processed Image data href vaild  signal
output  wire[23:0]  O_rgb
);

// 使能同步: 仅在I_vs为1时才允许切换，避免帧中间切换导致错位
reg en_sync;
always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n)
        en_sync <= 1'b0;
    else if (I_vs)
        en_sync <= I_en;
end

// R通道3x3窗口
wire        r_en;
wire [7:0]  r_p11, r_p12, r_p13;
wire [7:0]  r_p21, r_p22, r_p23;
wire [7:0]  r_p31, r_p32, r_p33;

// G通道3x3窗口
wire        g_en;
wire [7:0]  g_p11, g_p12, g_p13;
wire [7:0]  g_p21, g_p22, g_p23;
wire [7:0]  g_p31, g_p32, g_p33;

// B通道3x3窗口
wire        b_en;
wire [7:0]  b_p11, b_p12, b_p13;
wire [7:0]  b_p21, b_p22, b_p23;
wire [7:0]  b_p31, b_p32, b_p33;

// R通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_r_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[23:16]),
    .o_en       (r_en),
    .o_temp_11  (r_p11),
    .o_temp_12  (r_p12),
    .o_temp_13  (r_p13),
    .o_temp_21  (r_p21),
    .o_temp_22  (r_p22),
    .o_temp_23  (r_p23),
    .o_temp_31  (r_p31),
    .o_temp_32  (r_p32),
    .o_temp_33  (r_p33)
);

// G通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_g_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[15:8]),
    .o_en       (g_en),
    .o_temp_11  (g_p11),
    .o_temp_12  (g_p12),
    .o_temp_13  (g_p13),
    .o_temp_21  (g_p21),
    .o_temp_22  (g_p22),
    .o_temp_23  (g_p23),
    .o_temp_31  (g_p31),
    .o_temp_32  (g_p32),
    .o_temp_33  (g_p33)
);

// B通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_b_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[7:0]),
    .o_en       (b_en),
    .o_temp_11  (b_p11),
    .o_temp_12  (b_p12),
    .o_temp_13  (b_p13),
    .o_temp_21  (b_p21),
    .o_temp_22  (b_p22),
    .o_temp_23  (b_p23),
    .o_temp_31  (b_p31),
    .o_temp_32  (b_p32),
    .o_temp_33  (b_p33)
);

// 高斯核: [1,2,1; 2,4,2; 1,2,1] / 16
// 第一级: 行内加权求和
reg [11:0] R_sum1, R_sum2, R_sum3;
reg [11:0] G_sum1, G_sum2, G_sum3;
reg [11:0] B_sum1, B_sum2, B_sum3;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        R_sum1 <= 12'd0;  R_sum2 <= 12'd0;  R_sum3 <= 12'd0;
        G_sum1 <= 12'd0;  G_sum2 <= 12'd0;  G_sum3 <= 12'd0;
        B_sum1 <= 12'd0;  B_sum2 <= 12'd0;  B_sum3 <= 12'd0;
    end
    else if (!en_sync) begin
        R_sum1 <= 12'd0;  R_sum2 <= 12'd0;  R_sum3 <= 12'd0;
        G_sum1 <= 12'd0;  G_sum2 <= 12'd0;  G_sum3 <= 12'd0;
        B_sum1 <= 12'd0;  B_sum2 <= 12'd0;  B_sum3 <= 12'd0;
    end
    else begin
        R_sum1 <= r_p11 + r_p12*2 + r_p13;
        R_sum2 <= r_p21*2 + r_p22*4 + r_p23*2;
        R_sum3 <= r_p31 + r_p32*2 + r_p33;

        G_sum1 <= g_p11 + g_p12*2 + g_p13;
        G_sum2 <= g_p21*2 + g_p22*4 + g_p23*2;
        G_sum3 <= g_p31 + g_p32*2 + g_p33;

        B_sum1 <= b_p11 + b_p12*2 + b_p13;
        B_sum2 <= b_p21*2 + b_p22*4 + b_p23*2;
        B_sum3 <= b_p31 + b_p32*2 + b_p33;
    end
end

// 第二级: 三行加权之和
reg [11:0] R_sum, G_sum, B_sum;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        R_sum <= 12'd0;
        G_sum <= 12'd0;
        B_sum <= 12'd0;
    end
    else if (!en_sync) begin
        R_sum <= 12'd0;
        G_sum <= 12'd0;
        B_sum <= 12'd0;
    end
    else begin
        R_sum <= R_sum1 + R_sum2 + R_sum3;
        G_sum <= G_sum1 + G_sum2 + G_sum3;
        B_sum <= B_sum1 + B_sum2 + B_sum3;
    end
end

// 第三级: 除以16 (右移4位)
reg [7:0]  R_temp, G_temp, B_temp;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        R_temp <= 8'd0;
        G_temp <= 8'd0;
        B_temp <= 8'd0;
    end
    else if (!en_sync) begin
        R_temp <= 8'd0;
        G_temp <= 8'd0;
        B_temp <= 8'd0;
    end
    else begin
        R_temp <= R_sum[11:4];
        G_temp <= G_sum[11:4];
        B_temp <= B_sum[11:4];
    end
end

// 同步信号延迟
// DE 延迟链从 r_en（模板输出）开始，高斯计算 3 级流水线：行加权→列求和→右移除法
localparam DLY_CLK = 3;
reg [DLY_CLK-1:0] vs_dly;
reg [DLY_CLK-1:0] hs_dly;
reg [DLY_CLK-1:0] de_dly;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        vs_dly <= 0;
        hs_dly <= 0;
        de_dly <= 0;
    end
    else if (!en_sync) begin
        vs_dly <= 0;
        hs_dly <= 0;
        de_dly <= 0;
    end
    else begin
        vs_dly <= {vs_dly[DLY_CLK-2:0], I_vs};
        hs_dly <= {hs_dly[DLY_CLK-2:0], I_hs};
        de_dly <= {de_dly[DLY_CLK-2:0], r_en};
    end
end

wire gau_vs  = vs_dly[DLY_CLK-1];
wire gau_hs  = hs_dly[DLY_CLK-1];
wire gau_de  = de_dly[DLY_CLK-1];
wire [23:0] gau_rgb = {R_temp, G_temp, B_temp};

// 直通寄存(一级)
reg        bypass_vs;
reg        bypass_hs;
reg        bypass_de;
reg [23:0] bypass_rgb;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        bypass_vs  <= 1'b0;
        bypass_hs  <= 1'b0;
        bypass_de  <= 1'b0;
        bypass_rgb <= 24'd0;
    end else begin
        bypass_vs  <= I_vs;
        bypass_hs  <= I_hs;
        bypass_de  <= I_de;
        bypass_rgb <= I_rgb;
    end
end

// MUX输出: 使能时选高斯滤波，否则选直通
assign O_vs  = en_sync ? gau_vs   : bypass_vs;
assign O_hs  = en_sync ? gau_hs   : bypass_hs;
assign O_de  = en_sync ? gau_de   : bypass_de;
assign O_rgb = en_sync ? gau_rgb  : bypass_rgb;

endmodule

//======================================================================// >> laplacian_sharpen.v
`timescale 1ns/1ns

module laplacian_sharpen#(
parameter   [13:0]  H_SIZE = 1920,
parameter   [13:0]  V_SIZE = 1080
)
(
    //global clock
input   wire        I_vtc_clk,      //cmos video pixel clock
input   wire        I_rst_n,        //global reset
input   wire        I_en,           //使能: 1=Laplacian锐化, 0=直通(寄存一级)
input   wire        I_vs,           //Prepared Image data vsync valid signal
input   wire        I_hs,           //Prepared Image data hsync valid signal
input   wire        I_de,           //Prepared Image data href vaild  signal
input   wire[23:0]  I_rgb,          //Prepared Image data 24 Bit RGB Data

output  wire        O_vs,           //Processed Image data vsync valid signal
output  wire        O_hs,           //Processed Image data hsync valid signal
output  wire        O_de,           //Processed Image data href vaild  signal
output  wire[23:0]  O_rgb
);

// 使能同步: 仅在I_vs为1时才允许切换，避免帧中间切换导致错位
reg en_sync;
always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n)
        en_sync <= 1'b0;
    else if (I_vs)
        en_sync <= I_en;
end

// R通道3x3窗口
wire        r_en;
wire [7:0]  r_p11, r_p12, r_p13;
wire [7:0]  r_p21, r_p22, r_p23;
wire [7:0]  r_p31, r_p32, r_p33;

// G通道3x3窗口
wire        g_en;
wire [7:0]  g_p11, g_p12, g_p13;
wire [7:0]  g_p21, g_p22, g_p23;
wire [7:0]  g_p31, g_p32, g_p33;

// B通道3x3窗口
wire        b_en;
wire [7:0]  b_p11, b_p12, b_p13;
wire [7:0]  b_p21, b_p22, b_p23;
wire [7:0]  b_p31, b_p32, b_p33;

// R通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_r_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[23:16]),
    .o_en       (r_en),
    .o_temp_11  (r_p11),
    .o_temp_12  (r_p12),
    .o_temp_13  (r_p13),
    .o_temp_21  (r_p21),
    .o_temp_22  (r_p22),
    .o_temp_23  (r_p23),
    .o_temp_31  (r_p31),
    .o_temp_32  (r_p32),
    .o_temp_33  (r_p33)
);

// G通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_g_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[15:8]),
    .o_en       (g_en),
    .o_temp_11  (g_p11),
    .o_temp_12  (g_p12),
    .o_temp_13  (g_p13),
    .o_temp_21  (g_p21),
    .o_temp_22  (g_p22),
    .o_temp_23  (g_p23),
    .o_temp_31  (g_p31),
    .o_temp_32  (g_p32),
    .o_temp_33  (g_p33)
);

// B通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_b_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[7:0]),
    .o_en       (b_en),
    .o_temp_11  (b_p11),
    .o_temp_12  (b_p12),
    .o_temp_13  (b_p13),
    .o_temp_21  (b_p21),
    .o_temp_22  (b_p22),
    .o_temp_23  (b_p23),
    .o_temp_31  (b_p31),
    .o_temp_32  (b_p32),
    .o_temp_33  (b_p33)
);

// Laplacian锐化核: [0,-1,0; -1,5,-1; 0,-1,0]
// output = 5*P22 - P12 - P21 - P23 - P32
reg [11:0] R_sum1, G_sum1, B_sum1;  // 4邻域之和
reg [11:0] R_sum2, G_sum2, B_sum2;  // 5*P22
reg [7:0]  R_temp, G_temp, B_temp;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        R_sum1 <= 12'd0;  R_sum2 <= 12'd0;
        G_sum1 <= 12'd0;  G_sum2 <= 12'd0;
        B_sum1 <= 12'd0;  B_sum2 <= 12'd0;
    end
    else if (!en_sync) begin  // 禁用时复位
        R_sum1 <= 12'd0;  R_sum2 <= 12'd0;
        G_sum1 <= 12'd0;  G_sum2 <= 12'd0;
        B_sum1 <= 12'd0;  B_sum2 <= 12'd0;
    end
    else if (r_en) begin
        // R通道Laplacian锐化
        R_sum1 <= r_p12 + r_p21 + r_p23 + r_p32;
        R_sum2 <= {2'd0, r_p22, 2'd0} + {4'd0, r_p22};

        // G通道Laplacian锐化
        G_sum1 <= g_p12 + g_p21 + g_p23 + g_p32;
        G_sum2 <= {2'd0, g_p22, 2'd0} + {4'd0, g_p22};

        // B通道Laplacian锐化
        B_sum1 <= b_p12 + b_p21 + b_p23 + b_p32;
        B_sum2 <= {2'd0, b_p22, 2'd0} + {4'd0, b_p22};
    end
end

// 第二级: 饱和运算
wire [11:0] R_sum = R_sum2 - R_sum1;
wire [11:0] G_sum = G_sum2 - G_sum1;
wire [11:0] B_sum = B_sum2 - B_sum1;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        R_temp <= 8'd0;
        G_temp <= 8'd0;
        B_temp <= 8'd0;
    end
    else if (!en_sync) begin
        R_temp <= 8'd0;
        G_temp <= 8'd0;
        B_temp <= 8'd0;
    end
    else begin
        R_temp <= (R_sum2 < R_sum1) ? 8'd0 : (R_sum[11:8] > 0 ? 8'hff : R_sum[7:0]);
        G_temp <= (G_sum2 < G_sum1) ? 8'd0 : (G_sum[11:8] > 0 ? 8'hff : G_sum[7:0]);
        B_temp <= (B_sum2 < B_sum1) ? 8'd0 : (B_sum[11:8] > 0 ? 8'hff : B_sum[7:0]);
    end
end

// 同步信号延迟
// DE 延迟链从 r_en（模板输出）开始，Laplacian 计算 2 级流水线：4邻域求和→饱和运算
localparam DLY_CLK = 2;
reg [DLY_CLK-1:0] vs_dly;
reg [DLY_CLK-1:0] hs_dly;
reg [DLY_CLK-1:0] de_dly;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        vs_dly <= 0;
        hs_dly <= 0;
        de_dly <= 0;
    end
    else if (!en_sync) begin  // 禁用时复位延迟链
        vs_dly <= 0;
        hs_dly <= 0;
        de_dly <= 0;
    end
    else begin
        vs_dly <= {vs_dly[DLY_CLK-2:0], I_vs};
        hs_dly <= {hs_dly[DLY_CLK-2:0], I_hs};
        de_dly <= {de_dly[DLY_CLK-2:0], r_en};
    end
end

wire lap_vs  = vs_dly[DLY_CLK-1];
wire lap_hs  = hs_dly[DLY_CLK-1];
wire lap_de  = de_dly[DLY_CLK-1];
wire [23:0] lap_rgb = {R_temp, G_temp, B_temp};

// 直通寄存(一级)
reg        bypass_vs;
reg        bypass_hs;
reg        bypass_de;
reg [23:0] bypass_rgb;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        bypass_vs  <= 1'b0;
        bypass_hs  <= 1'b0;
        bypass_de  <= 1'b0;
        bypass_rgb <= 24'd0;
    end else begin
        bypass_vs  <= I_vs;
        bypass_hs  <= I_hs;
        bypass_de  <= I_de;
        bypass_rgb <= I_rgb;
    end
end

// MUX输出: 使能时选Laplacian锐化，否则选直通
assign O_vs  = en_sync ? lap_vs   : bypass_vs;
assign O_hs  = en_sync ? lap_hs   : bypass_hs;
assign O_de  = en_sync ? lap_de   : bypass_de;
assign O_rgb = en_sync ? lap_rgb  : bypass_rgb;

endmodule

//======================================================================// >> sobel.v
`timescale 1ns/1ns

module sobel#(
parameter   [13:0]  H_SIZE = 1920,
parameter   [13:0]  V_SIZE = 1080
)
(
    //global clock
input   wire        I_vtc_clk,      //cmos video pixel clock
input   wire        I_rst_n,        //global reset
input   wire        I_en,           //使能: 1=Sobel处理, 0=直通(寄存一级)
input   wire        I_vs,           //Prepared Image data vsync valid signal
input   wire        I_hs,           //Prepared Image data hsync valid signal
input   wire        I_de,           //Prepared Image data href vaild  signal
input   wire[23:0]  I_rgb,          //Prepared Image data 24 Bit RGB Data

output  wire        O_vs,           //Processed Image data vsync valid signal
output  wire        O_hs,           //Processed Image data hsync valid signal
output  wire        O_de,           //Processed Image data href vaild  signal
output  wire[23:0]  O_rgb
);

// 使能同步: 仅在I_vs为1时才允许切换，避免帧中间切换导致错位
reg en_sync;
always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n)
        en_sync <= 1'b0;
    else if (I_vs)
        en_sync <= I_en;
end

// R通道3x3窗口
wire        r_en;
wire [7:0]  r_p11, r_p12, r_p13;
wire [7:0]  r_p21, r_p22, r_p23;
wire [7:0]  r_p31, r_p32, r_p33;

// G通道3x3窗口
wire        g_en;
wire [7:0]  g_p11, g_p12, g_p13;
wire [7:0]  g_p21, g_p22, g_p23;
wire [7:0]  g_p31, g_p32, g_p33;

// B通道3x3窗口
wire        b_en;
wire [7:0]  b_p11, b_p12, b_p13;
wire [7:0]  b_p21, b_p22, b_p23;
wire [7:0]  b_p31, b_p32, b_p33;

// R通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_r_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[23:16]),
    .o_en       (r_en),
    .o_temp_11  (r_p11),
    .o_temp_12  (r_p12),
    .o_temp_13  (r_p13),
    .o_temp_21  (r_p21),
    .o_temp_22  (r_p22),
    .o_temp_23  (r_p23),
    .o_temp_31  (r_p31),
    .o_temp_32  (r_p32),
    .o_temp_33  (r_p33)
);

// G通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_g_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[15:8]),
    .o_en       (g_en),
    .o_temp_11  (g_p11),
    .o_temp_12  (g_p12),
    .o_temp_13  (g_p13),
    .o_temp_21  (g_p21),
    .o_temp_22  (g_p22),
    .o_temp_23  (g_p23),
    .o_temp_31  (g_p31),
    .o_temp_32  (g_p32),
    .o_temp_33  (g_p33)
);

// B通道3x3模板
image_template_3x3 #(
    .H_ACTIVE   (H_SIZE),
    .V_ACTIVE   (V_SIZE)
) u_b_template (
    .i_clk      (I_vtc_clk),
    .i_rst_n    (I_rst_n & en_sync),
    .i_en       (I_de),
    .i_data     (I_rgb[7:0]),
    .o_en       (b_en),
    .o_temp_11  (b_p11),
    .o_temp_12  (b_p12),
    .o_temp_13  (b_p13),
    .o_temp_21  (b_p21),
    .o_temp_22  (b_p22),
    .o_temp_23  (b_p23),
    .o_temp_31  (b_p31),
    .o_temp_32  (b_p32),
    .o_temp_33  (b_p33)
);

reg [9:0] R_gx1,R_gx3,R_gy1,R_gy3,R_gx,R_gy;
reg [10:0] R_g;
reg [7:0] R_temp;

reg [9:0] G_gx1,G_gx3,G_gy1,G_gy3,G_gx,G_gy;
reg [10:0] G_g;
reg [7:0] G_temp;

reg [9:0] B_gx1,B_gx3,B_gy1,B_gy3,B_gx,B_gy;
reg [10:0] B_g;
reg [7:0] B_temp;

always @ (posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        R_gx1 <=10'b0;
        R_gx3 <=10'b0;
        R_gy1 <=10'b0;
        R_gy3 <=10'b0;
        R_gx <=10'b0;
        R_gy <=10'b0;
        R_g <=11'b0;
        R_temp <= 8'b0;

        B_gx1 <=10'b0;
        B_gx3 <=10'b0;
        B_gy1 <=10'b0;
        B_gy3 <=10'b0;
        B_gx <=10'b0;
        B_gy <=10'b0;
        B_g <=11'b0;
        B_temp <= 8'b0;

        G_gx1 <=10'b0;
        G_gx3 <=10'b0;
        G_gy1 <=10'b0;
        G_gy3 <=10'b0;
        G_gx <=10'b0;
        G_gy <=10'b0;
        G_g <=11'b0;
        G_temp <= 8'b0;
    end
    else if (!en_sync) begin  // 禁用时复位Sobel计算寄存器
        R_gx1 <=10'b0;  R_gx3 <=10'b0;  R_gy1 <=10'b0;  R_gy3 <=10'b0;
        R_gx <=10'b0;   R_gy <=10'b0;   R_g <=11'b0;    R_temp <= 8'b0;

        G_gx1 <=10'b0;  G_gx3 <=10'b0;  G_gy1 <=10'b0;  G_gy3 <=10'b0;
        G_gx <=10'b0;   G_gy <=10'b0;   G_g <=11'b0;    G_temp <= 8'b0;

        B_gx1 <=10'b0;  B_gx3 <=10'b0;  B_gy1 <=10'b0;  B_gy3 <=10'b0;
        B_gx <=10'b0;   B_gy <=10'b0;   B_g <=11'b0;    B_temp <= 8'b0;
    end
    else if (r_en) begin
        // R通道Sobel计算
        R_gx1 <= r_p11 + (r_p21 << 1) + r_p31;
        R_gx3 <= r_p13 + (r_p23 << 1) + r_p33;
        R_gy1 <= r_p11 + (r_p12 << 1) + r_p13;
        R_gy3 <= r_p31 + (r_p32 << 1) + r_p33;
        R_gx <= R_gx1 > R_gx3 ? R_gx1 - R_gx3 : R_gx3 - R_gx1;
        R_gy <= R_gy1 > R_gy3 ? R_gy1 - R_gy3 : R_gy3 - R_gy1;
        R_g <= R_gx + R_gy;
        R_temp <= R_g > 8'hff ? 8'hff : R_g[7:0];

        // G通道Sobel计算
        G_gx1 <= g_p11 + (g_p21 << 1) + g_p31;
        G_gx3 <= g_p13 + (g_p23 << 1) + g_p33;
        G_gy1 <= g_p11 + (g_p12 << 1) + g_p13;
        G_gy3 <= g_p31 + (g_p32 << 1) + g_p33;
        G_gx <= G_gx1 > G_gx3 ? G_gx1 - G_gx3 : G_gx3 - G_gx1;
        G_gy <= G_gy1 > G_gy3 ? G_gy1 - G_gy3 : G_gy3 - G_gy1;
        G_g <= G_gx + G_gy;
        G_temp <= G_g > 8'hff ? 8'hff : G_g[7:0];

        // B通道Sobel计算
        B_gx1 <= b_p11 + (b_p21 << 1) + b_p31;
        B_gx3 <= b_p13 + (b_p23 << 1) + b_p33;
        B_gy1 <= b_p11 + (b_p12 << 1) + b_p13;
        B_gy3 <= b_p31 + (b_p32 << 1) + b_p33;
        B_gx <= B_gx1 > B_gx3 ? B_gx1 - B_gx3 : B_gx3 - B_gx1;
        B_gy <= B_gy1 > B_gy3 ? B_gy1 - B_gy3 : B_gy3 - B_gy1;
        B_g <= B_gx + B_gy;
        B_temp <= B_g > 8'hff ? 8'hff : B_g[7:0];
    end
end

// 同步信号延迟
// DE 延迟链从 r_en（模板输出）开始，Sobel 计算 4 级流水线：部分和→差值→梯度→饱和
localparam DLY_CLK = 4;
reg [DLY_CLK-1:0] vs_dly;
reg [DLY_CLK-1:0] hs_dly;
reg [DLY_CLK-1:0] de_dly;

always @ (posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        vs_dly <= 0;
        hs_dly <= 0;
        de_dly <= 0;
    end
    else if (!en_sync) begin  // 禁用时复位延迟链
        vs_dly <= 0;
        hs_dly <= 0;
        de_dly <= 0;
    end
    else begin
        vs_dly <= {vs_dly[DLY_CLK-2:0], I_vs};
        hs_dly <= {hs_dly[DLY_CLK-2:0], I_hs};
        de_dly <= {de_dly[DLY_CLK-2:0], r_en};
    end
end

wire sobel_vs = vs_dly[DLY_CLK-1];
wire sobel_hs = hs_dly[DLY_CLK-1];
wire sobel_de = de_dly[DLY_CLK-1];
wire [23:0] sobel_rgb = {R_temp, G_temp, B_temp};

// 直通寄存(一级)
reg        bypass_vs;
reg        bypass_hs;
reg        bypass_de;
reg [23:0] bypass_rgb;

always @(posedge I_vtc_clk or negedge I_rst_n) begin
    if (!I_rst_n) begin
        bypass_vs  <= 1'b0;
        bypass_hs  <= 1'b0;
        bypass_de  <= 1'b0;
        bypass_rgb <= 24'd0;
    end else begin
        bypass_vs  <= I_vs;
        bypass_hs  <= I_hs;
        bypass_de  <= I_de;
        bypass_rgb <= I_rgb;
    end
end

// MUX输出: 使能时选Sobel，否则选直通
assign O_vs  = en_sync ? sobel_vs  : bypass_vs;
assign O_hs  = en_sync ? sobel_hs  : bypass_hs;
assign O_de  = en_sync ? sobel_de  : bypass_de;
assign O_rgb = en_sync ? sobel_rgb : bypass_rgb;

endmodule

//======================================================================// >> image_template_3x3.v
`timescale 1ns / 1ns
//==============================================================================
// 模块名称: image_template_3x3
// 功能描述: 3x3 图像窗口提取
//           - 使用 shift_register（循环 RAM）实现行缓存
//           - 自动延迟 H_ACTIVE 拍，无需 v_cnt 控制读写使能
//==============================================================================

module image_template_3x3
(
    input   wire            i_clk,
    input   wire            i_rst_n,
    input   wire            i_en,
    input   wire [7:0]      i_data,
    output  reg             o_en,
    output  reg [7:0]       o_temp_11,
    output  reg [7:0]       o_temp_12,
    output  reg [7:0]       o_temp_13,
    output  reg [7:0]       o_temp_21,
    output  reg [7:0]       o_temp_22,
    output  reg [7:0]       o_temp_23,
    output  reg [7:0]       o_temp_31,
    output  reg [7:0]       o_temp_32,
    output  reg [7:0]       o_temp_33
);
parameter  H_ACTIVE = 1280;
parameter  V_ACTIVE = 720;

reg [10:0] h_cnt;
reg [10:0] v_cnt;

// 行计数
always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n)
        h_cnt <= 11'd0;
    else if (i_en) begin
        if (h_cnt == H_ACTIVE - 1'b1)
            h_cnt <= 11'd0;
        else
            h_cnt <= h_cnt + 11'd1;
    end
end

// 场计数
always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n)
        v_cnt <= 11'd0;
    else if (h_cnt == H_ACTIVE - 1'b1) begin
        if (v_cnt == V_ACTIVE - 1'b1)
            v_cnt <= 11'd0;
        else
            v_cnt <= v_cnt + 11'd1;
    end
end

// 行缓存：shift_register 自动延迟 H_ACTIVE 拍（一整行）
wire [7:0] line_buf_1_out;
wire [7:0] line_buf_2_out;

shift_register #(
    .DATA_WIDTH (8),
    .DATA_DEPTH (H_ACTIVE),
    .ADDR_WIDTH (11)
) u_sr_1 (
    .clk   (i_clk),
    .rst_n (i_rst_n),
    .clken (i_en),
    .din   (i_data),
    .dout  (line_buf_1_out)
);

shift_register #(
    .DATA_WIDTH (8),
    .DATA_DEPTH (H_ACTIVE),
    .ADDR_WIDTH (11)
) u_sr_2 (
    .clk   (i_clk),
    .rst_n (i_rst_n),
    .clken (i_en),
    .din   (line_buf_1_out),
    .dout  (line_buf_2_out)
);

// 3x3 窗口形成
always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
        o_temp_11 <= 8'd0; o_temp_12 <= 8'd0; o_temp_13 <= 8'd0;
        o_temp_21 <= 8'd0; o_temp_22 <= 8'd0; o_temp_23 <= 8'd0;
        o_temp_31 <= 8'd0; o_temp_32 <= 8'd0; o_temp_33 <= 8'd0;
    end
    else if (i_en) begin
        if (v_cnt == 0) begin
            if (h_cnt == 0) begin
                o_temp_11 <= i_data; o_temp_12 <= i_data; o_temp_13 <= i_data;
                o_temp_21 <= i_data; o_temp_22 <= i_data; o_temp_23 <= i_data;
                o_temp_31 <= i_data; o_temp_32 <= i_data; o_temp_33 <= i_data;
            end
            else begin
                o_temp_11 <= o_temp_12; o_temp_12 <= o_temp_13; o_temp_13 <= i_data;
                o_temp_21 <= o_temp_22; o_temp_22 <= o_temp_23; o_temp_23 <= i_data;
                o_temp_31 <= o_temp_32; o_temp_32 <= o_temp_33; o_temp_33 <= i_data;
            end
        end
        else if (v_cnt == 1) begin
            if (h_cnt == 0) begin
                o_temp_11 <= line_buf_1_out; o_temp_12 <= line_buf_1_out; o_temp_13 <= line_buf_1_out;
                o_temp_21 <= line_buf_1_out; o_temp_22 <= line_buf_1_out; o_temp_23 <= line_buf_1_out;
                o_temp_31 <= i_data;         o_temp_32 <= i_data;         o_temp_33 <= i_data;
            end
            else begin
                o_temp_11 <= o_temp_12;       o_temp_12 <= o_temp_13;       o_temp_13 <= line_buf_1_out;
                o_temp_21 <= o_temp_22;       o_temp_22 <= o_temp_23;       o_temp_23 <= line_buf_1_out;
                o_temp_31 <= o_temp_32;       o_temp_32 <= o_temp_33;       o_temp_33 <= i_data;
            end
        end
        else begin
            if (h_cnt == 0) begin
                o_temp_11 <= line_buf_2_out; o_temp_12 <= line_buf_2_out; o_temp_13 <= line_buf_2_out;
                o_temp_21 <= line_buf_1_out; o_temp_22 <= line_buf_1_out; o_temp_23 <= line_buf_1_out;
                o_temp_31 <= i_data;         o_temp_32 <= i_data;         o_temp_33 <= i_data;
            end
            else begin
                o_temp_11 <= o_temp_12;       o_temp_12 <= o_temp_13;       o_temp_13 <= line_buf_2_out;
                o_temp_21 <= o_temp_22;       o_temp_22 <= o_temp_23;       o_temp_23 <= line_buf_1_out;
                o_temp_31 <= o_temp_32;       o_temp_32 <= o_temp_33;       o_temp_33 <= i_data;
            end
        end
    end
end

// 输出使能
always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n)
        o_en <= 1'b0;
    else
        o_en <= i_en;
end

endmodule

//======================================================================// >> shift_register.v
//  by CrazyBird
module shift_register
#(
    parameter DATA_WIDTH = 8    ,
    parameter ADDR_WIDTH = 11   ,
    parameter DATA_DEPTH = 1280 ,
    parameter DELAY_NUM  = 0
)(
    input  wire                     clk     ,
    input  wire                     rst_n   ,
    input  wire                     clken   ,
    input  wire [DATA_WIDTH-1:0]    din     ,   
    output wire [DATA_WIDTH-1:0]    dout    
);
//----------------------------------------------------------------------
localparam BRAM_DEPTH = DATA_DEPTH + 1;
localparam INIT_ADDR  = DATA_DEPTH - DELAY_NUM;

//----------------------------------------------------------------------
reg             [ADDR_WIDTH-1:0]    bram_waddr;
reg             [ADDR_WIDTH-1:0]    bram_raddr;

always @(posedge clk or negedge rst_n)
begin
    if(rst_n == 1'b0)
    begin
        bram_waddr <= INIT_ADDR;
        bram_raddr <= 0;
    end
    else
    begin
        if(clken == 1'b1)
        begin
            if(bram_waddr < DATA_DEPTH)
                bram_waddr <= bram_waddr + 1'b1;
            else
                bram_waddr <= 0;
            if(bram_raddr < DATA_DEPTH)
                bram_raddr <= bram_raddr + 1'b1;
            else
                bram_raddr <= 0;
        end
        else
        begin
            bram_waddr <= bram_waddr;
            bram_raddr <= bram_raddr;
        end
    end
end




//----------------------------------------------------------------------
wire            [DATA_WIDTH-1:0]    bram_wdata;
wire                                bram_wenb;
reg            [DATA_WIDTH-1:0]    bram_rdata;

assign bram_wdata = din;
assign bram_wenb  = clken;


localparam ADDR_MSB = 2 ** ADDR_WIDTH - 1; 
reg 	[DATA_WIDTH-1:0] 	r_ram[ADDR_MSB:0]; 

always @(posedge clk) begin
	if(bram_wenb) begin
		r_ram[bram_waddr] <= bram_wdata; 
	end else begin
	end
end
always @(posedge clk) begin
	bram_rdata <= r_ram[bram_raddr]; 
end


assign dout = bram_rdata;

endmodule
