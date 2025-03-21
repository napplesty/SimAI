#!/bin/bash

# 设置调试环境变量
export HETERDIST_DEBUG_LOG=1  # 启用HeterDist详细日志
export AS_SEND_LAT=2          # 设置发送延迟

# 设置输出文件
LOG_FILE="debug_log.txt"
ERROR_LOG="debug_error.txt"
GDB_COMMANDS="gdb_commands.txt"

# 创建GDB命令文件
cat > $GDB_COMMANDS << EOF
# 设置断点
break MockNcclGroup::genFlowModelsHeterDist
break get_flow_model_heterdist

# 运行程序
run -t 1 -w ./example/microAllReduce.txt -n ./Spectrum-X_8g_8gps_400Gbps_H100 -c ./astra-sim-alibabacloud/inputs/config/SimAI.conf

# 当程序停在断点时，可以使用以下命令
# 打印变量
# print rank
# print data_size
# print algo_name
# print heter_flows.size()
# print rank2pflowmodels.size()

# 检查所有rank的流模型
# commands 1
#   printf "检查所有rank的流模型\\n"
#   print heter_flows.size()
#   print rank2pflowmodels.size()
#   for (auto& [r, flows] : rank2pflowmodels) {
#     printf("Rank %d has %zu flows\\n", r, flows->size());
#   }
#   continue
# end

# 继续执行
# continue

# 单步执行
# next

# 进入函数
# step

# 查看调用栈
# backtrace

# 查看当前位置的代码
# list

# 添加更多自定义命令...

EOF

echo "开始GDB调试运行..." | tee -a $LOG_FILE

# 选择调试模式
echo "请选择调试模式:"
echo "1) 使用GDB交互式调试"
echo "2) 使用GDB批处理模式调试"
echo "3) 不使用GDB，直接运行程序"
read -p "请输入选项 (1-3): " DEBUG_MODE

case $DEBUG_MODE in
  1)
    # GDB交互式模式
    echo "启动GDB交互式调试..." | tee -a $LOG_FILE
    sudo gdb --args ./bin/SimAI_simulator \
      -t 1 \
      -w ./example/microAllReduce.txt \
      -n ./DCN+SingleToR_16g_8gps_400Gbps_H100  \
      -c ./astra-sim-alibabacloud/inputs/config/SimAI.conf
    ;;
  2)
    # GDB批处理模式
    echo "启动GDB批处理模式调试..." | tee -a $LOG_FILE
    sudo gdb -x $GDB_COMMANDS --args ./bin/SimAI_simulator \
      -t 1 \
      -w ./example/microAllReduce.txt \
      -n ./Spectrum-X_8g_8gps_400Gbps_H100 \
      -c ./astra-sim-alibabacloud/inputs/config/SimAI.conf \
      2> $ERROR_LOG | tee -a $LOG_FILE
    ;;
  3)
    # 直接运行程序
    echo "执行模拟命令（不使用GDB）..." | tee -a $LOG_FILE
    sudo ./bin/SimAI_simulator \
      -t 1 \
      -w ./example/microAllReduce.txt \
      -n ./Spectrum-X_8g_8gps_400Gbps_H100 \
      -c ./astra-sim-alibabacloud/inputs/config/SimAI.conf \
      2> $ERROR_LOG | tee -a $LOG_FILE
    ;;
  *)
    echo "无效选项，默认使用直接运行模式" | tee -a $LOG_FILE
    sudo ./bin/SimAI_simulator \
      -t 1 \
      -w ./example/microAllReduce.txt \
      -n ./Spectrum-X_8g_8gps_400Gbps_H100 \
      -c ./astra-sim-alibabacloud/inputs/config/SimAI.conf \
      2> $ERROR_LOG | tee -a $LOG_FILE
    ;;
esac

# 检查运行结果
if [ $? -eq 0 ]; then
  echo "模拟成功完成！" | tee -a $LOG_FILE
else
  echo "模拟运行出错，请查看错误日志: $ERROR_LOG" | tee -a $LOG_FILE
fi

# 提取关键信息
echo "提取流模型信息..." | tee -a $LOG_FILE
grep -A 20 "Flow Models Information" $LOG_FILE > flow_models.txt

# 检查是否有多个rank的流模型
echo "检查所有rank的流模型..." | tee -a $LOG_FILE
for i in {0..7}; do
  grep -A 5 "Rank $i has" $LOG_FILE >> rank_flows.txt
done

echo "调试完成。详细日志保存在 $LOG_FILE"
echo "流模型信息保存在 flow_models.txt"
echo "各rank流模型信息保存在 rank_flows.txt"
echo "GDB命令保存在 $GDB_COMMANDS"
