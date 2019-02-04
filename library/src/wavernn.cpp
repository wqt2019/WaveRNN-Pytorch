/*
Copyright 2019 Eugene Ingerman

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <stdio.h>
#include <cmath>
#include "wavernn.h"

inline Vectorf sigmoid( const Vectorf& v )
{
    //TODO: optimize this
    //maybe use one of these approximations: https://stackoverflow.com/questions/10732027/fast-sigmoid-algorithm
    Vectorf y = 1.f / ( 1.f + Eigen::exp( - v.array()));
    return y;
}

inline Vectorf tanh( const Vectorf& v )
{
    //TODO: optimize this
    Vectorf y = Eigen::tanh( v.array() );
    return y;
}

BaseLayer *TorchLayer::loadNext(FILE *fd)
{
    TorchLayer::Header header;
    fread(&header, sizeof(TorchLayer::Header), 1, fd);

    printf("Loading: %s\n", header.name);

    switch( header.layerType ){

    case TorchLayer::Header::LayerType::Linear:
    {
        impl = new LinearLayer();
        impl->loadNext(fd);
        return impl;
    }
    break;

    case TorchLayer::Header::LayerType::GRU:
    {
        impl = new GRULayer();
        impl->loadNext(fd);
        return impl;
    }
    break;

    case TorchLayer::Header::LayerType::Conv1d:
    {
        impl = new Conv1dLayer();
        impl->loadNext(fd);
        return impl;
    }
    case TorchLayer::Header::LayerType::Conv2d:{
        impl = new Conv2dLayer();
        impl->loadNext(fd);
        return impl;
    }
    case TorchLayer::Header::LayerType::BatchNorm1d:
    {
        impl = new BatchNorm1dLayer();
        impl->loadNext(fd);
        return impl;
    }
    default:
        return nullptr;
    }
}


LinearLayer* LinearLayer::loadNext(FILE *fd)
{
    LinearLayer::Header header;
    fread( &header, sizeof(LinearLayer::Header), 1, fd);
    assert(header.elSize==4 or header.elSize==2);

    mat.read(fd, header.elSize, header.nRows, header.nCols); //read compressed array

    bias.resize(header.nRows);
    fread(bias.data(), header.elSize, header.nRows, fd);
}

Vectorf LinearLayer::apply(const Vectorf &x)
{
    return sigmoid(mat*x);
}

GRULayer* GRULayer::loadNext(FILE *fd)
{
    GRULayer::Header header;
    fread( &header, sizeof(GRULayer::Header), 1, fd);
    assert(header.elSize==4 or header.elSize==2);

    nRows = header.nHidden;
    nCols = header.nInput;

    b_ir.resize(header.nHidden);
    b_iz.resize(header.nHidden);
    b_in.resize(header.nHidden);

    b_hr.resize(header.nHidden);
    b_hz.resize(header.nHidden);
    b_hn.resize(header.nHidden);


    W_ir.read( fd, header.elSize, header.nHidden, header.nInput);
    W_iz.read( fd, header.elSize, header.nHidden, header.nInput);
    W_in.read( fd, header.elSize, header.nHidden, header.nInput);

    W_hr.read( fd, header.elSize, header.nHidden, header.nHidden);
    W_hz.read( fd, header.elSize, header.nHidden, header.nHidden);
    W_hn.read( fd, header.elSize, header.nHidden, header.nHidden);

    fread(b_ir.data(), header.elSize, header.nHidden, fd);
    fread(b_iz.data(), header.elSize, header.nHidden, fd);
    fread(b_in.data(), header.elSize, header.nHidden, fd);

    fread(b_hr.data(), header.elSize, header.nHidden, fd);
    fread(b_hz.data(), header.elSize, header.nHidden, fd);
    fread(b_hn.data(), header.elSize, header.nHidden, fd);
}


Vectorf GRULayer::apply(const Vectorf &x, const Vectorf &hx)
{
    Vectorf r, z, n, hout;

    r = sigmoid( W_ir*x + b_ir + W_hr*hx + b_hr);
    z = sigmoid( W_iz*x + b_iz + W_hz*hx + b_hz);
    n = tanh( W_in*x + b_in + (r.array() * (W_hn*hx + b_hn).array()).matrix());
    hout = (1.f-z.array()) * n.array() + z.array() * hx.array();
    return hout;
}


Vectorf CompMatrix::operator*(const Vectorf &x)
{
    Vectorf y(nRows);
    assert(nCols == x.size());

    int indexPos = 0;
    int weightPos = 0;
    int row = 0;
    float sum = 0;

    while( row < nRows ){
        int idxPos = index[indexPos++];
        if( idxPos != ROW_END_MARKER ){
            //TODO: vectorize this multiplication
            int col = SPARSE_GROUP_SIZE*idxPos;

            assert( col+SPARSE_GROUP_SIZE <= x.size() );
            assert( weightPos+SPARSE_GROUP_SIZE <= weight.size());

            for(int i=0; i<SPARSE_GROUP_SIZE; ++i)
                sum += weight(weightPos++) * x(col+i);

        } else { //end of row. assign output and continue to the next row.
            assert( row < nRows );
            y(row) = sum;
            sum = 0.f;
            ++row;
        }
    }
    return y;
}

Conv1dLayer *Conv1dLayer::loadNext(FILE *fd)
{
    Conv1dLayer::Header header;
    fread( &header, sizeof(Conv1dLayer::Header), 1, fd);
    assert(header.elSize==4 or header.elSize==2);
    hasBias = header.useBias;
    inChannels = header.inChannels;
    outChannels = header.outChannels;
    nKernel = header.kernelSize;

    weight.resize(outChannels);
    for(int i=0; i<outChannels; ++i){
        weight[i].resize(inChannels, nKernel);
        fread(weight[i].data(), header.elSize, inChannels*nKernel, fd);
    }

    if( hasBias ){
        bias.resize(outChannels);
        fread(bias.data(), header.elSize, outChannels, fd);
    }
}

Matrixf Conv1dLayer::apply(const Matrixf &x)
{
    int convDim = x.cols()-nKernel+1;
    Matrixf y(outChannels, convDim);

    for(int outIdx = 0; outIdx<outChannels; ++outIdx){
        for(int kernIdx = 0; kernIdx < convDim; ++kernIdx ){
            y(outIdx, kernIdx) = ( x.block(0, kernIdx, inChannels, nKernel).cwiseProduct( weight[outIdx] ) ).sum();
        }
    }

    if( hasBias ){
        y.colwise() += bias;
    }

    return y;
}

Conv2dLayer *Conv2dLayer::loadNext(FILE *fd)
{
    assert(0);
}

Matrixf Conv2dLayer::apply(const Matrixf &x)
{
    assert(0);
}

BatchNorm1dLayer *BatchNorm1dLayer::loadNext(FILE *fd)
{
    assert(0);
}

Vectorf BatchNorm1dLayer::apply(const Vectorf &x)
{
    assert(0);
}
